/* Copyright (c) 2002-2009 Dovecot authors, see the included COPYING file */

#include "common.h"
#include "array.h"
#include "hash.h"
#include "fd-close-on-exec.h"
#include "eacces-error.h"
#include "env-util.h"
#include "base64.h"
#include "str.h"
#include "network.h"
#include "mountpoint.h"
#include "restrict-access.h"
#include "restrict-process-size.h"
#include "home-expand.h"
#include "var-expand.h"
#include "mail-process.h"
#include "login-process.h"
#include "log.h"

#include <stdlib.h>
#include <unistd.h>
#include <grp.h>
#include <syslog.h>
#include <sys/stat.h>

#ifdef HAVE_SYS_RESOURCE_H
#  include <sys/resource.h>
#endif

/* Timeout chdir() completely after this many seconds */
#define CHDIR_TIMEOUT 30
/* Give a warning about chdir() taking a while if it took longer than this
   many seconds to finish. */
#define CHDIR_WARN_SECS 10

struct mail_process_group {
	/* process.type + user + remote_ip identifies this process group */
	struct child_process process;
	char *user;
	struct ip_addr remote_ip;

	/* processes array acts also as refcount */
	ARRAY_DEFINE(processes, pid_t);
};

/* type+user -> struct mail_process_group */
static struct hash_table *mail_process_groups;
static unsigned int mail_process_count = 0;

static unsigned int mail_process_group_hash(const void *p)
{
	const struct mail_process_group *group = p;

	return str_hash(group->user) ^ group->process.type ^
		net_ip_hash(&group->remote_ip);
}

static int mail_process_group_cmp(const void *p1, const void *p2)
{
	const struct mail_process_group *group1 = p1, *group2 = p2;
	int ret;

	ret = strcmp(group1->user, group2->user);
	if (ret == 0)
		ret = group1->process.type - group2->process.type;
	if (ret == 0 && !net_ip_compare(&group1->remote_ip, &group2->remote_ip))
		ret = -1;
	return ret;
}

static struct mail_process_group *
mail_process_group_lookup(enum process_type type, const char *user,
			  const struct ip_addr *ip)
{
	struct mail_process_group lookup_group;

	lookup_group.process.type = type;
	lookup_group.user = t_strdup_noconst(user);
	lookup_group.remote_ip = *ip;

	return hash_table_lookup(mail_process_groups, &lookup_group);
}

static struct mail_process_group *
mail_process_group_create(enum process_type type, const char *user,
			  const struct ip_addr *ip)
{
	struct mail_process_group *group;

	group = i_new(struct mail_process_group, 1);
	group->process.type = type;
	group->user = i_strdup(user);
	group->remote_ip = *ip;

	i_array_init(&group->processes, 10);
	hash_table_insert(mail_process_groups, group, group);
	return group;
}

static void
mail_process_group_add(struct mail_process_group *group, pid_t pid)
{
	mail_process_count++;
	array_append(&group->processes, &pid, 1);
	child_process_add(pid, &group->process);
}

static void mail_process_group_free(struct mail_process_group *group)
{
	array_free(&group->processes);
	i_free(group->user);
	i_free(group);
}

static bool validate_uid_gid(struct settings *set, uid_t uid, gid_t gid,
			     const char *user)
{
	if (uid == 0) {
		i_error("user %s: Logins with UID 0 not permitted", user);
		return FALSE;
	}

	if (set->login_uid == uid && master_uid != uid) {
		i_error("user %s: Logins with login_user's UID %s "
			"not permitted (see http://wiki.dovecot.org/UserIds).",
			user, dec2str(uid));
		return FALSE;
	}

	if (uid < (uid_t)set->first_valid_uid ||
	    (set->last_valid_uid != 0 && uid > (uid_t)set->last_valid_uid)) {
		i_error("user %s: Logins with UID %s not permitted "
			"(see first_valid_uid in config file).",
			user, dec2str(uid));
		return FALSE;
	}

	if (gid < (gid_t)set->first_valid_gid ||
	    (set->last_valid_gid != 0 && gid > (gid_t)set->last_valid_gid)) {
		i_error("user %s: Logins for users with primary group ID %s "
			"not permitted (see first_valid_gid in config file).",
			user, dec2str(gid));
		return FALSE;
	}

	return TRUE;
}

static bool validate_chroot(struct settings *set, const char *dir)
{
	const char *const *chroot_dirs;

	if (*dir == '\0')
		return FALSE;

	if (*set->valid_chroot_dirs == '\0')
		return FALSE;

	chroot_dirs = t_strsplit(set->valid_chroot_dirs, ":");
	while (*chroot_dirs != NULL) {
		if (**chroot_dirs != '\0' &&
		    strncmp(dir, *chroot_dirs, strlen(*chroot_dirs)) == 0)
			return TRUE;
		chroot_dirs++;
	}

	return FALSE;
}

static const struct var_expand_table *
get_var_expand_table(const char *protocol,
		     const char *user, const char *home,
		     const char *local_ip, const char *remote_ip,
		     pid_t pid, uid_t uid)
{
#define VAR_EXPAND_HOME_IDX 4
	static struct var_expand_table static_tab[] = {
		{ 'u', NULL, "user" },
		{ 'n', NULL, "username" },
		{ 'd', NULL, "domain" },
		{ 's', NULL, "service" },
		{ 'h', NULL, "home" },
		{ 'l', NULL, "lip" },
		{ 'r', NULL, "rip" },
		{ 'p', NULL, "pid" },
		{ 'i', NULL, "uid" },
		{ '\0', NULL, NULL }
	};
	struct var_expand_table *tab;

	tab = t_malloc(sizeof(static_tab));
	memcpy(tab, static_tab, sizeof(static_tab));

	tab[0].value = user;
	tab[1].value = user == NULL ? NULL : t_strcut(user, '@');
	tab[2].value = user == NULL ? NULL : strchr(user, '@');
	if (tab[2].value != NULL) tab[2].value++;
	tab[3].value = t_str_ucase(protocol);
	tab[VAR_EXPAND_HOME_IDX].value = home;
	tab[5].value = local_ip;
	tab[6].value = remote_ip;
	tab[7].value = dec2str(pid);
	tab[8].value = dec2str(uid);

	return tab;
}

static bool
has_missing_used_home(const char *str, const struct var_expand_table *table)
{
	i_assert(table[VAR_EXPAND_HOME_IDX].key == 'h');

	return table[VAR_EXPAND_HOME_IDX].value == NULL &&
		var_has_key(str, 'h', "home");
}

static const char *
expand_mail_env(const char *env, const struct var_expand_table *table)
{
	string_t *str;
	const char *p;

	str = t_str_new(256);

	/* it's either type:data or just data */
	p = strchr(env, ':');
	if (p != NULL) {
		while (env != p) {
			str_append_c(str, *env);
			env++;
		}

		str_append_c(str, *env++);
	}

	if (has_missing_used_home(env, table)) {
		i_fatal("userdb didn't return a home directory, "
			"but mail location used it (%%h): %s", env);
	}

	/* expand %vars */
	var_expand(str, env, table);
	return str_c(str);
}

static void
env_put_namespace(struct namespace_settings *ns, const char *default_location,
		  const struct var_expand_table *table)
{
	const char *location;
	unsigned int i;
	string_t *str;

	if (default_location == NULL)
		default_location = "";

	for (i = 1; ns != NULL; i++, ns = ns->next) {
		location = *ns->location != '\0' ? ns->location :
			default_location;
		location = expand_mail_env(location, table);
		env_put(t_strdup_printf("NAMESPACE_%u=%s", i, location));

		if (ns->separator != NULL) {
			env_put(t_strdup_printf("NAMESPACE_%u_SEP=%s",
						i, ns->separator));
		}
		if (ns->type != NULL) {
			env_put(t_strdup_printf("NAMESPACE_%u_TYPE=%s",
						i, ns->type));
		}
		if (ns->alias_for != NULL) {
			env_put(t_strdup_printf("NAMESPACE_%u_ALIAS=%s",
						i, ns->alias_for));
		}
		if (ns->prefix != NULL) {
			/* expand variables, eg. ~%u/ can be useful */
			str = t_str_new(256);
			str_printfa(str, "NAMESPACE_%u_PREFIX=", i);
			var_expand(str, ns->prefix, table);
			env_put(str_c(str));
		}
		if (ns->inbox)
			env_put(t_strdup_printf("NAMESPACE_%u_INBOX=1", i));
		if (ns->hidden)
			env_put(t_strdup_printf("NAMESPACE_%u_HIDDEN=1", i));
		if (strcmp(ns->list, "no") != 0) {
			env_put(t_strdup_printf("NAMESPACE_%u_LIST=%s",
						i, ns->list));
		}
		if (ns->subscriptions)
			env_put(t_strdup_printf("NAMESPACE_%u_SUBSCRIPTIONS=1",
						i));
	}
}

static void
mail_process_set_environment(struct settings *set, const char *mail,
			     const struct var_expand_table *var_expand_table,
			     bool exec_mail)
{
	const char *const *envs;
	string_t *str;
	unsigned int i, count;

	env_put(t_strconcat("MAIL_CACHE_FIELDS=",
			    set->mail_cache_fields, NULL));
	env_put(t_strconcat("MAIL_NEVER_CACHE_FIELDS=",
			    set->mail_never_cache_fields, NULL));
	env_put(t_strdup_printf("MAIL_CACHE_MIN_MAIL_COUNT=%u",
				set->mail_cache_min_mail_count));
	env_put(t_strdup_printf("MAILBOX_IDLE_CHECK_INTERVAL=%u",
				set->mailbox_idle_check_interval));
	env_put(t_strdup_printf("MAIL_MAX_KEYWORD_LENGTH=%u",
				set->mail_max_keyword_length));

	if (set->protocol == MAIL_PROTOCOL_IMAP) {
		env_put(t_strdup_printf("IMAP_MAX_LINE_LENGTH=%u",
					set->imap_max_line_length));
		if (*set->imap_capability != '\0') {
			env_put(t_strconcat("IMAP_CAPABILITY=",
					    set->imap_capability, NULL));
		}
		env_put(t_strconcat("IMAP_CLIENT_WORKAROUNDS=",
				    set->imap_client_workarounds, NULL));
		env_put(t_strconcat("IMAP_LOGOUT_FORMAT=",
				    set->imap_logout_format, NULL));
		env_put(t_strconcat("IMAP_ID_SEND=", set->imap_id_send, NULL));
		env_put(t_strconcat("IMAP_ID_LOG=", set->imap_id_log, NULL));
	}
	if (set->protocol == MAIL_PROTOCOL_POP3) {
		env_put(t_strconcat("POP3_CLIENT_WORKAROUNDS=",
				    set->pop3_client_workarounds, NULL));
		env_put(t_strconcat("POP3_LOGOUT_FORMAT=",
				    set->pop3_logout_format, NULL));
		if (set->pop3_no_flag_updates)
			env_put("POP3_NO_FLAG_UPDATES=1");
		if (set->pop3_reuse_xuidl)
			env_put("POP3_REUSE_XUIDL=1");
		if (set->pop3_enable_last)
			env_put("POP3_ENABLE_LAST=1");
		if (set->pop3_lock_session)
			env_put("POP3_LOCK_SESSION=1");
	}

	/* We care about POP3 UIDL format in all process types */
	env_put(t_strconcat("POP3_UIDL_FORMAT=", set->pop3_uidl_format, NULL));

	if (set->mail_save_crlf)
		env_put("MAIL_SAVE_CRLF=1");
	if (set->mmap_disable)
		env_put("MMAP_DISABLE=1");
	if (set->dotlock_use_excl)
		env_put("DOTLOCK_USE_EXCL=1");
	if (set->fsync_disable)
		env_put("FSYNC_DISABLE=1");
	if (set->mail_nfs_storage)
		env_put("MAIL_NFS_STORAGE=1");
	if (set->mail_nfs_index)
		env_put("MAIL_NFS_INDEX=1");
	if (set->mailbox_list_index_disable)
		env_put("MAILBOX_LIST_INDEX_DISABLE=1");
	if (set->maildir_stat_dirs)
		env_put("MAILDIR_STAT_DIRS=1");
	if (set->maildir_copy_with_hardlinks)
		env_put("MAILDIR_COPY_WITH_HARDLINKS=1");
	if (set->maildir_copy_preserve_filename)
		env_put("MAILDIR_COPY_PRESERVE_FILENAME=1");
	if (set->mail_debug)
		env_put("DEBUG=1");
	if (set->mail_full_filesystem_access)
		env_put("FULL_FILESYSTEM_ACCESS=1");
	if (set->mbox_dirty_syncs)
		env_put("MBOX_DIRTY_SYNCS=1");
	if (set->mbox_very_dirty_syncs)
		env_put("MBOX_VERY_DIRTY_SYNCS=1");
	if (set->mbox_lazy_writes)
		env_put("MBOX_LAZY_WRITES=1");
	/* when we're not certain that the log fd points to the master
	   process's log pipe (dump-capability, --exec-mail), don't let
	   the imap process listen for stderr since it might break
	   (e.g. epoll_ctl() gives EPERM). */
	if (set->shutdown_clients && !exec_mail)
		env_put("STDERR_CLOSE_SHUTDOWN=1");
	(void)umask(set->umask);

	env_put(t_strconcat("LOCK_METHOD=", set->lock_method, NULL));
	env_put(t_strconcat("MBOX_READ_LOCKS=", set->mbox_read_locks, NULL));
	env_put(t_strconcat("MBOX_WRITE_LOCKS=", set->mbox_write_locks, NULL));
	env_put(t_strdup_printf("MBOX_LOCK_TIMEOUT=%u",
				set->mbox_lock_timeout));
	env_put(t_strdup_printf("MBOX_DOTLOCK_CHANGE_TIMEOUT=%u",
				set->mbox_dotlock_change_timeout));
	env_put(t_strdup_printf("MBOX_MIN_INDEX_SIZE=%u",
				set->mbox_min_index_size));

	env_put(t_strdup_printf("DBOX_ROTATE_SIZE=%u",
				set->dbox_rotate_size));
	env_put(t_strdup_printf("DBOX_ROTATE_MIN_SIZE=%u",
				set->dbox_rotate_min_size));
	env_put(t_strdup_printf("DBOX_ROTATE_DAYS=%u",
				set->dbox_rotate_days));

	if (*set->mail_plugins != '\0') {
		env_put(t_strconcat("MAIL_PLUGIN_DIR=",
				    set->mail_plugin_dir, NULL));
		env_put(t_strconcat("MAIL_PLUGINS=", set->mail_plugins, NULL));
	}

	/* user given environment - may be malicious. virtual_user comes from
	   auth process, but don't trust that too much either. Some auth
	   mechanism might allow leaving extra data there. */
	if ((mail == NULL || *mail == '\0') && *set->mail_location != '\0')
		mail = expand_mail_env(set->mail_location, var_expand_table);
	env_put(t_strconcat("MAIL=", mail, NULL));

	if (set->server->namespaces != NULL) {
		env_put_namespace(set->server->namespaces,
				  mail, var_expand_table);
	}

	str = t_str_new(256);
	envs = array_get(&set->plugin_envs, &count);
	i_assert((count % 2) == 0);
	for (i = 0; i < count; i += 2) {
		str_truncate(str, 0);
		var_expand(str, envs[i+1], var_expand_table);

		if (has_missing_used_home(envs[i+1], var_expand_table)) {
			i_error("userdb didn't return a home directory, "
				"but it's used in plugin setting %s: %s",
				envs[i], envs[i+1]);
		}

		env_put(t_strconcat(t_str_ucase(envs[i]), "=",
				    str_c(str), NULL));
	}
}

void mail_process_exec(const char *protocol, const char **args)
{
	struct server_settings *server = settings_root;
	const struct var_expand_table *var_expand_table;
	struct settings *set;
	const char *executable;

	if (strcmp(protocol, "ext") == 0) {
		/* external binary. section contains path for it. */
		if (*args == NULL)
			i_fatal("External binary parameter not given");
		set = server->defaults;
		executable = *args;
	} else {
		const char *section = *args;

		if (section != NULL) {
			for (; server != NULL; server = server->next) {
				if (strcmp(server->name, section) == 0)
					break;
			}
			if (server == NULL)
				i_fatal("Section not found: '%s'", section);
		}

		if (strcmp(protocol, "imap") == 0)
			set = server->imap;
		else if (strcmp(protocol, "pop3") == 0)
			set = server->pop3;
		else
			i_fatal("Unknown protocol: '%s'", protocol);
		executable = set->mail_executable;
		args = NULL;
	}

	var_expand_table =
		get_var_expand_table(protocol, getenv("USER"), getenv("HOME"),
				     getenv("TCPLOCALIP"),
				     getenv("TCPREMOTEIP"),
				     getpid(), geteuid());

	/* set up logging */
	env_put(t_strconcat("LOG_TIMESTAMP=", set->log_timestamp, NULL));
	if (*set->log_path == '\0')
		env_put("USE_SYSLOG=1");
	else
		env_put(t_strconcat("LOGFILE=", set->log_path, NULL));
	if (*set->info_log_path != '\0')
		env_put(t_strconcat("INFOLOGFILE=", set->info_log_path, NULL));
	if (*set->mail_log_prefix != '\0') {
		string_t *str = t_str_new(256);

		str_append(str, "LOG_PREFIX=");
		var_expand(str, set->mail_log_prefix, var_expand_table);
		env_put(str_c(str));
	}

	mail_process_set_environment(set, getenv("MAIL"), var_expand_table,
				     TRUE);
	if (args == NULL)
		client_process_exec(executable, "");
	else
		client_process_exec_argv(executable, args);

	i_fatal_status(FATAL_EXEC, "execv(%s) failed: %m", executable);
}

static void nfs_warn_if_found(const char *mail, const char *full_home_dir)
{
	struct mountpoint point;
	const char *path;

	if (mail == NULL || *mail == '\0')
		path = full_home_dir;
	else {
		path = strstr(mail, ":INDEX=");
		if (path != NULL) {
			/* indexes set separately */
			path += 7;
			if (strncmp(path, "MEMORY", 6) == 0)
				return;
		} else {
			path = strchr(mail, ':');
			if (path == NULL) {
				/* autodetection for path */
				path = mail;
			} else {
				/* format:path */
				path++;
			}
		}
		path = home_expand_tilde(t_strcut(path, ':'), full_home_dir);
	}

	if (mountpoint_get(path, pool_datastack_create(), &point) <= 0)
		return;

	if (point.type == NULL || strcasecmp(point.type, "NFS") != 0)
		return;

	i_fatal("Mailbox indexes in %s are in NFS mount. You must set "
		"mail_nfs_index=yes (and mail_nfs_storage=yes) to avoid index corruptions. "
		"If you're sure this check was wrong, set nfs_check=no.", path);
}

enum master_login_status
create_mail_process(enum process_type process_type, struct settings *set,
		    const struct mail_login_request *request,
		    const char *user, const char *const *args,
		    const unsigned char *data, bool dump_capability,
		    pid_t *pid_r)
{
	const struct var_expand_table *var_expand_table;
	const char *p, *addr, *mail, *chroot_dir, *home_dir, *full_home_dir;
	const char *system_user, *master_user;
	struct mail_process_group *process_group;
	char title[1024];
	struct log_io *log;
	string_t *str;
	pid_t pid;
	uid_t uid;
	gid_t gid;
	ARRAY_DEFINE(extra_args, const char *);
	unsigned int i, len, count, left, process_count, throttle;
	int ret, log_fd, nice_value, chdir_errno;
	bool home_given, nfs_check;

	i_assert(process_type == PROCESS_TYPE_IMAP ||
		 process_type == PROCESS_TYPE_POP3);

	if (mail_process_count == set->max_mail_processes) {
		i_error("Maximum number of mail processes exceeded "
			"(see max_mail_processes setting)");
		return MASTER_LOGIN_STATUS_INTERNAL_ERROR;
	}

	t_array_init(&extra_args, 16);
	mail = home_dir = chroot_dir = system_user = ""; master_user = NULL;
	uid = (uid_t)-1; gid = (gid_t)-1; nice_value = 0;
	home_given = FALSE;
	for (; *args != NULL; args++) {
		if (strncmp(*args, "home=", 5) == 0) {
			home_dir = *args + 5;
			home_given = TRUE;
		} else if (strncmp(*args, "mail=", 5) == 0)
			mail = *args + 5;
		else if (strncmp(*args, "chroot=", 7) == 0)
			chroot_dir = *args + 7;
		else if (strncmp(*args, "nice=", 5) == 0)
			nice_value = atoi(*args + 5);
		else if (strncmp(*args, "system_user=", 12) == 0)
			system_user = *args + 12;
		else if (strncmp(*args, "uid=", 4) == 0) {
			if (uid != (uid_t)-1) {
				i_error("uid specified multiple times for %s",
					user);
				return MASTER_LOGIN_STATUS_INTERNAL_ERROR;
			}
			uid = (uid_t)strtoul(*args + 4, NULL, 10);
		} else if (strncmp(*args, "gid=", 4) == 0) {
			gid = (gid_t)strtoul(*args + 4, NULL, 10);
		} else if (strncmp(*args, "master_user=", 12) == 0) {
			const char *arg = *args;

			master_user = arg + 12;
			array_append(&extra_args, &arg, 1);
		} else {
			const char *arg = *args;
			array_append(&extra_args, &arg, 1);
		}
	}

	/* check process limit for this user, but not if this is a master
	   user login. */
	process_group = dump_capability ? NULL :
		mail_process_group_lookup(process_type, user,
					  &request->remote_ip);
	process_count = process_group == NULL ? 0 :
		array_count(&process_group->processes);
	if (process_count >= set->mail_max_userip_connections &&
	    set->mail_max_userip_connections != 0 &&
	    master_user == NULL)
		return MASTER_LOGIN_STATUS_MAX_CONNECTIONS;

	/* if uid/gid wasn't returned, use the defaults */
	if (uid == (uid_t)-1) {
		uid = set->mail_uid_t;
		if (uid == (uid_t)-1) {
			i_error("User %s is missing UID (see mail_uid setting)",
				user);
			return MASTER_LOGIN_STATUS_INTERNAL_ERROR;
		}
	}
	if (gid == (gid_t)-1) {
		gid = set->mail_gid_t;
		if (gid == (gid_t)-1) {
			i_error("User %s is missing GID (see mail_gid setting)",
				user);
			return MASTER_LOGIN_STATUS_INTERNAL_ERROR;
		}
	}

	if (*chroot_dir == '\0' && *set->valid_chroot_dirs != '\0' &&
	    (p = strstr(home_dir, "/./")) != NULL) {
		/* wu-ftpd like <chroot>/./<home> - check only if there's even
		   a possibility of using them (non-empty valid_chroot_dirs)*/
		chroot_dir = t_strdup_until(home_dir, p);
		home_dir = p + 2;
	} else if (*chroot_dir != '\0' && *home_dir != '/') {
		/* home directories should never be relative, but force this
		   with chroots. */
		home_dir = t_strconcat("/", home_dir, NULL);
	}

	if (!dump_capability) {
		if (!validate_uid_gid(set, uid, gid, user))
			return MASTER_LOGIN_STATUS_INTERNAL_ERROR;
	}

	if (*chroot_dir != '\0') {
		if (!validate_chroot(set, chroot_dir)) {
			i_error("Invalid chroot directory '%s' (user %s) "
				"(see valid_chroot_dirs setting)",
				chroot_dir, user);
			return MASTER_LOGIN_STATUS_INTERNAL_ERROR;
		}
	} else if (*set->mail_chroot != '\0') {
		/* mail_chroot setting's value doesn't need to be in
		   valid_chroot_dirs. */
		chroot_dir = set->mail_chroot;
	}
	if (*chroot_dir != '\0' && set->mail_drop_priv_before_exec) {
		i_error("Can't chroot to directory '%s' (user %s) "
			"with mail_drop_priv_before_exec=yes",
			chroot_dir, user);
		return MASTER_LOGIN_STATUS_INTERNAL_ERROR;
	}
	len = strlen(chroot_dir);
	if (len > 2 && strcmp(chroot_dir + len - 2, "/.") == 0 &&
	    strncmp(home_dir, chroot_dir, len - 2) == 0) {
		/* strip chroot dir from home dir */
		home_dir += len - 2;
	}

	if (!dump_capability) {
		throttle = set->mail_debug ? 0 :
			set->mail_log_max_lines_per_sec;
		log_fd = log_create_pipe(&log, throttle);
		if (log_fd == -1)
			return MASTER_LOGIN_STATUS_INTERNAL_ERROR;
	} else {
		log = NULL;
		log_fd = dup(STDERR_FILENO);
		if (log_fd == -1) {
			i_error("dup() failed: %m");
			return MASTER_LOGIN_STATUS_INTERNAL_ERROR;
		}
		fd_close_on_exec(log_fd, TRUE);
	}

	/* See if we need to do the initial NFS check. We want to do this only
	   once, so the check code needs to be before fork(). */
	if (set->nfs_check && !set->mail_nfs_index && !dump_capability) {
		set->nfs_check = FALSE;
		nfs_check = TRUE;
	} else {
		nfs_check = FALSE;
	}

	pid = fork();
	if (pid < 0) {
		i_error("fork() failed: %m");
		(void)close(log_fd);
		return MASTER_LOGIN_STATUS_INTERNAL_ERROR;
	}

	var_expand_table =
		get_var_expand_table(process_names[process_type],
				     user, home_given ? home_dir : NULL,
				     net_ip2addr(&request->local_ip),
				     net_ip2addr(&request->remote_ip),
				     pid != 0 ? pid : getpid(), uid);
	str = t_str_new(128);

	if (pid != 0) {
		/* master */
		var_expand(str, set->mail_log_prefix, var_expand_table);

		if (!dump_capability) {
			log_set_prefix(log, str_c(str));
			log_set_pid(log, pid);
			if (process_group == NULL) {
				process_group = mail_process_group_create(
							process_type, user,
							&request->remote_ip);
			}
			mail_process_group_add(process_group, pid);
		}
		(void)close(log_fd);
		*pid_r = pid;
		return MASTER_LOGIN_STATUS_OK;
	}

#ifdef HAVE_SETPRIORITY
	if (nice_value != 0) {
		if (setpriority(PRIO_PROCESS, 0, nice_value) < 0)
			i_error("setpriority(%d) failed: %m", nice_value);
	}
#endif

	if (!dump_capability) {
		str_append(str, "master-");
		var_expand(str, set->mail_log_prefix, var_expand_table);
		log_set_prefix(log, str_c(str));
	}

	child_process_init_env();

	/* move the client socket into stdin and stdout fds, log to stderr */
	if (dup2(dump_capability ? null_fd : request->fd, 0) < 0)
		i_fatal("dup2(stdin) failed: %m");
	if (dup2(request->fd, 1) < 0)
		i_fatal("dup2(stdout) failed: %m");
	if (dup2(log_fd, 2) < 0)
		i_fatal("dup2(stderr) failed: %m");

	for (i = 0; i < 3; i++)
		fd_close_on_exec(i, FALSE);

	/* setup environment - set the most important environment first
	   (paranoia about filling up environment without noticing) */
	restrict_access_set_env(system_user, uid, gid, set->mail_priv_gid_t,
				dump_capability ? "" : chroot_dir,
				set->first_valid_gid, set->last_valid_gid,
				set->mail_access_groups);

	restrict_process_size(set->mail_process_size, (unsigned int)-1);

	if (dump_capability)
		env_put("DUMP_CAPABILITY=1");

	if ((*home_dir == '\0' && *chroot_dir == '\0') || dump_capability) {
		full_home_dir = "";
		ret = -1;
	} else {
		full_home_dir = *chroot_dir == '\0' ? home_dir :
			t_strconcat(chroot_dir, home_dir, NULL);
		/* NOTE: if home directory is NFS-mounted, we might not
		   have access to it as root. Change the effective UID and GID
		   temporarily to make it work. */
		if (uid != master_uid) {
			if (setegid(gid) < 0)
				i_fatal("setegid(%s) failed: %m", dec2str(gid));
			if (seteuid(uid) < 0)
				i_fatal("seteuid(%s) failed: %m", dec2str(uid));
		}

		alarm(CHDIR_TIMEOUT);
		ret = chdir(full_home_dir);
		chdir_errno = errno;
		if ((left = alarm(0)) < CHDIR_TIMEOUT - CHDIR_WARN_SECS) {
			i_warning("chdir(%s) blocked for %u secs",
				  full_home_dir, CHDIR_TIMEOUT - left);
		}

		/* Change UID back. No need to change GID back, it doesn't
		   really matter. */
		if (uid != master_uid && seteuid(master_uid) < 0)
			i_fatal("seteuid(%s) failed: %m", dec2str(master_uid));

		/* If user's home directory doesn't exist and we're not
		   trying to chroot anywhere, fallback to /tmp as the mails
		   could be stored elsewhere. The ENOTDIR check is mostly for
		   /dev/null home directory. */
		if (ret < 0 && (*chroot_dir != '\0' ||
				!(ENOTFOUND(chdir_errno) ||
				  chdir_errno == EINTR))) {
			errno = chdir_errno;
			if (errno != EACCES) {
				i_fatal("chdir(%s) failed with uid %s: %m",
					full_home_dir, dec2str(uid));
			} else {
				i_fatal("%s", eacces_error_get("chdir",
							       full_home_dir));
			}
		}
	}
	if (ret < 0) {
		/* We still have to change to some directory where we have
		   rx-access. /tmp should exist everywhere. */
		if (chdir("/tmp") < 0)
			i_fatal("chdir(/tmp) failed: %m");
	}

	mail_process_set_environment(set, mail, var_expand_table,
				     dump_capability);

	/* extra args. uppercase key value. */
	args = array_get(&extra_args, &count);
	for (i = 0; i < count; i++) {
		if (*args[i] == '=') {
			/* Should be caught by dovecot-auth already */
			i_fatal("Userdb returned data with empty key (%s)",
				args[i]);
		}
		p = strchr(args[i], '=');
		if (p == NULL) {
			/* boolean */
			env_put(t_strconcat(t_str_ucase(args[i]), "=1", NULL));

		} else {
			/* key=value */
			env_put(t_strconcat(t_str_ucase(
				t_strdup_until(args[i], p)), p, NULL));
		}
	}

	if (nfs_check) {
		/* ideally we should check all of the namespaces,
		   but for now don't bother. */
		const char *mail_location = getenv("NAMESPACE_1");

		if (mail_location == NULL)
			mail_location = getenv("MAIL");
		nfs_warn_if_found(mail_location, full_home_dir);
	}

	env_put("LOGGED_IN=1");
	if (*home_dir != '\0')
		env_put(t_strconcat("HOME=", home_dir, NULL));
	env_put(t_strconcat("USER=", user, NULL));

	addr = net_ip2addr(&request->remote_ip);
	env_put(t_strconcat("IP=", addr, NULL));
	env_put(t_strconcat("LOCAL_IP=", net_ip2addr(&request->local_ip), NULL));

	i_assert(request->cmd_tag_size <= request->data_size);
	if (request->cmd_tag_size > 0) {
		env_put(t_strconcat("IMAPLOGINTAG=",
			t_strndup(data, request->cmd_tag_size), NULL));
	}

	if (request->data_size > request->cmd_tag_size) {
		str_truncate(str, 0);
		str_append(str, "CLIENT_INPUT=");
		base64_encode(data + request->cmd_tag_size,
			      request->data_size - request->cmd_tag_size, str);
		env_put(str_c(str));
	}

	if (!set->verbose_proctitle)
		title[0] = '\0';
	else {
		if (addr == NULL)
			addr = "??";

		i_snprintf(title, sizeof(title), "[%s %s]", user, addr);
	}

	/* make sure we don't leak syslog fd, but do it last so that
	   any errors above will be logged */
	closelog();

	if (set->mail_drop_priv_before_exec) {
		restrict_access_by_env(TRUE);
		/* privileged GID is now only in saved-GID. if we want to
		   preserve it accross exec, it needs to be temporarily
		   in effective gid */
		restrict_access_use_priv_gid();
	}

	client_process_exec(set->mail_executable, title);
	i_fatal_status(FATAL_EXEC, "execv(%s) failed: %m",
		       set->mail_executable);

	/* not reached */
	return MASTER_LOGIN_STATUS_INTERNAL_ERROR;
}

static void
mail_process_destroyed(struct child_process *process,
		       pid_t pid, bool abnormal_exit ATTR_UNUSED)
{
	struct mail_process_group *group = (struct mail_process_group *)process;
	const pid_t *pids;
	unsigned int i, count;

	pids = array_get(&group->processes, &count);
	if (count == 1) {
		/* last process in this group */
		i_assert(pids[0] == pid);
		hash_table_remove(mail_process_groups, group);
		mail_process_group_free(group);
	} else {
		for (i = 0; i < count; i++) {
			if (pids[i] == pid)
				break;
		}
		i_assert(i != count);
		array_delete(&group->processes, i, 1);
	}

	mail_process_count--;
}

void mail_processes_init(void)
{
	mail_process_groups = hash_table_create(default_pool, default_pool, 0,
						mail_process_group_hash,
						mail_process_group_cmp);

	child_process_set_destroy_callback(PROCESS_TYPE_IMAP,
					   mail_process_destroyed);
	child_process_set_destroy_callback(PROCESS_TYPE_POP3,
					   mail_process_destroyed);
}

void mail_processes_deinit(void)
{
	/* we may still end up in mail_process_destroyed(), so don't free
	   anything. This deinit code needs a redesign.. */
}
