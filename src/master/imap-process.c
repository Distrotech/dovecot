/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "env-util.h"
#include "temp-string.h"
#include "restrict-access.h"
#include "restrict-process-size.h"

#include <stdlib.h>
#include <unistd.h>
#include <grp.h>
#include <syslog.h>
#include <sys/stat.h>

static unsigned int imap_process_count = 0;

static int validate_uid_gid(uid_t uid, gid_t gid)
{
	if (uid == 0) {
		i_error("imap process isn't allowed for root");
		return FALSE;
	}

	if (uid != 0 && gid == 0) {
		i_error("imap process isn't allowed to be in group 0");
		return FALSE;
	}

	if (uid < (uid_t)set_first_valid_uid ||
	    (set_last_valid_uid != 0 && uid > (uid_t)set_last_valid_uid)) {
		i_error("imap process isn't allowed to use UID %s",
			dec2str(uid));
		return FALSE;
	}

	if (gid < (gid_t)set_first_valid_gid ||
	    (set_last_valid_gid != 0 && gid > (gid_t)set_last_valid_gid)) {
		i_error("imap process isn't allowed to use "
			"GID %s (UID is %s)", dec2str(gid), dec2str(uid));
		return FALSE;
	}

	return TRUE;
}

static int validate_chroot(const char *dir)
{
	char *const *chroot_dirs;

	if (*dir == '\0')
		return TRUE;

	if (set_valid_chroot_dirs == NULL)
		return FALSE;

	chroot_dirs = t_strsplit(set_valid_chroot_dirs, ":");
	while (*chroot_dirs != NULL) {
		if (**chroot_dirs != '\0' &&
		    strncmp(dir, *chroot_dirs, strlen(*chroot_dirs)) == 0)
			return TRUE;
		chroot_dirs++;
	}

	return FALSE;
}

static const char *expand_mail_env(const char *env, const char *user,
				   const char *home)
{
	TempString *str;
	const char *p;

	str = t_string_new(256);

	/* it's either type:data or just data */
	p = strchr(env, ':');
	if (p != NULL) {
		while (env != p) {
			t_string_append_c(str, *env);
			env++;
		}

		t_string_append_c(str, *env++);
	}

	if (env[0] == '~' && env[1] == '/') {
		/* expand home */
		t_string_append(str, home);
		env++;
	}

	/* expand $U if found */
	for (; *env != '\0'; env++) {
		if (*env == '$' && env[1] == 'U') {
			t_string_append(str, user);
			env++;
		} else {
			t_string_append_c(str, *env);
		}
	}

	return str->str;
}

MasterReplyResult create_imap_process(int socket, IPADDR *ip,
				      const char *system_user,
				      const char *virtual_user,
				      uid_t uid, gid_t gid, const char *home,
				      int chroot, const char *mail,
				      const char *login_tag)
{
	static char *argv[] = { NULL, NULL, NULL };
	const char *host;
	char title[1024];
	pid_t pid;
	int i, err;

	if (imap_process_count == set_max_imap_processes) {
		i_error("Maximum number of imap processes exceeded");
		return MASTER_RESULT_INTERNAL_FAILURE;
	}

	if (!validate_uid_gid(uid, gid))
		return MASTER_RESULT_FAILURE;

	if (chroot && !validate_chroot(home))
		return MASTER_RESULT_FAILURE;

	pid = fork();
	if (pid < 0) {
		i_error("fork() failed: %m");
		return MASTER_RESULT_INTERNAL_FAILURE;
	}

	if (pid != 0) {
		/* master */
		imap_process_count++;
		PID_ADD_PROCESS_TYPE(pid, PROCESS_TYPE_IMAP);
		return MASTER_RESULT_SUCCESS;
	}

	clean_child_process();

	/* move the imap socket into stdin, stdout and stderr fds */
	for (i = 0; i < 3; i++) {
		if (dup2(socket, i) < 0)
			i_fatal("imap: dup2(%d) failed: %m", i);
	}

	if (close(socket) < 0)
		i_error("imap: close(imap client) failed: %m");

	/* setup environment - set the most important environment first
	   (paranoia about filling up environment without noticing) */
	restrict_access_set_env(system_user, uid, gid, chroot ? home : NULL);
	restrict_process_size(set_imap_process_size);

	env_put(t_strconcat("HOME=", home, NULL));
	env_put(t_strconcat("MAIL_CACHE_FIELDS=", set_mail_cache_fields, NULL));
	env_put(t_strconcat("MAIL_NEVER_CACHE_FIELDS=",
			    set_mail_never_cache_fields, NULL));
	env_put(t_strdup_printf("MAILBOX_CHECK_INTERVAL=%u",
				set_mailbox_check_interval));

	if (set_mail_save_crlf)
		env_put("MAIL_SAVE_CRLF=1");
	if (set_mail_read_mmaped)
		env_put("MAIL_READ_MMAPED=1");
	if (set_maildir_copy_with_hardlinks)
		env_put("MAILDIR_COPY_WITH_HARDLINKS=1");
	if (set_maildir_check_content_changes)
		env_put("MAILDIR_CHECK_CONTENT_CHANGES=1");
	if (set_overwrite_incompatible_index)
		env_put("OVERWRITE_INCOMPATIBLE_INDEX=1");
	if (umask(set_umask) != set_umask)
		i_fatal("Invalid umask: %o", set_umask);

	env_put(t_strconcat("MBOX_LOCKS=", set_mbox_locks, NULL));
	env_put(t_strdup_printf("MBOX_LOCK_TIMEOUT=%u", set_mbox_lock_timeout));
	env_put(t_strdup_printf("MBOX_DOTLOCK_CHANGE_TIMEOUT=%u",
				set_mbox_dotlock_change_timeout));
	if (set_mbox_read_dotlock)
		env_put("MBOX_READ_DOTLOCK=1");

	/* user given environment - may be malicious. virtual_user comes from
	   auth process, but don't trust that too much either. Some auth
	   mechanism might allow leaving extra data there. */
	if (mail == NULL && set_default_mail_env != NULL) {
		mail = expand_mail_env(set_default_mail_env,
				       virtual_user, home);
		env_put(t_strconcat("MAIL=", mail, NULL));
	}

	env_put(t_strconcat("MAIL=", mail, NULL));
	env_put(t_strconcat("USER=", virtual_user, NULL));
	env_put(t_strconcat("LOGIN_TAG=", login_tag, NULL));

	if (set_verbose_proctitle) {
		host = net_ip2host(ip);
		if (host == NULL)
			host = "??";

		i_snprintf(title, sizeof(title), "[%s %s]", virtual_user, host);
		argv[1] = title;
	}

	/* make sure we don't leak syslog fd, but do it last so that
	   any errors above will be logged */
	closelog();

	/* hide the path, it's ugly */
	argv[0] = strrchr(set_imap_executable, '/');
	if (argv[0] == NULL) argv[0] = set_imap_executable; else argv[0]++;

	execv(set_imap_executable, argv);
	err = errno;

	for (i = 0; i < 3; i++)
		(void)close(i);

	i_fatal_status(FATAL_EXEC, "execv(%s) failed: %m", set_imap_executable);

	/* not reached */
	return 0;
}

void imap_process_destroyed(pid_t pid __attr_unused__)
{
	imap_process_count--;
}
