/* Copyright (C) 2002-2003 Timo Sirainen */

#include "common.h"
#include "ioloop.h"
#include "network.h"
#include "lib-signals.h"
#include "restrict-access.h"
#include "fd-close-on-exec.h"
#include "process-title.h"
#include "randgen.h"
#include "module-dir.h"
#include "var-expand.h"
#include "dict-client.h"
#include "mail-storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>

#define IS_STANDALONE() \
        (getenv("LOGGED_IN") == NULL)

struct client_workaround_list {
	const char *name;
	enum client_workarounds num;
};

struct client_workaround_list client_workaround_list[] = {
	{ "outlook-no-nuls", WORKAROUND_OUTLOOK_NO_NULS },
	{ "oe-ns-eoh", WORKAROUND_OE_NS_EOH },
	{ NULL, 0 }
};

struct ioloop *ioloop;

void (*hook_mail_storage_created)(struct mail_storage *storage) = NULL;
void (*hook_client_created)(struct client **client) = NULL;

static struct module *modules;
static char log_prefix[128]; /* syslog() needs this to be permanent */
static struct io *log_io = NULL;

enum client_workarounds client_workarounds = 0;
bool enable_last_command = FALSE;
bool no_flag_updates = FALSE;
bool reuse_xuidl = FALSE;
bool lock_session = FALSE;
const char *uidl_format, *logout_format;
enum uidl_keys uidl_keymask;

static void sig_die(int signo, void *context __attr_unused__)
{
	/* warn about being killed because of some signal, except SIGINT (^C)
	   which is too common at least while testing :) */
	if (signo != SIGINT)
		i_warning("Killed with signal %d", signo);
	io_loop_stop(ioloop);
}

static void log_error_callback(void *context __attr_unused__)
{
	io_loop_stop(ioloop);
}

static void parse_workarounds(void)
{
        struct client_workaround_list *list;
	const char *env, *const *str;

	env = getenv("POP3_CLIENT_WORKAROUNDS");
	if (env == NULL)
		return;

	for (str = t_strsplit_spaces(env, " ,"); *str != NULL; str++) {
		list = client_workaround_list;
		for (; list->name != NULL; list++) {
			if (strcasecmp(*str, list->name) == 0) {
				client_workarounds |= list->num;
				break;
			}
		}
		if (list->name == NULL)
			i_fatal("Unknown client workaround: %s", *str);
	}
}

static enum uidl_keys parse_uidl_keymask(const char *format)
{
	enum uidl_keys mask = 0;

	for (; *format != '\0'; format++) {
		if (format[0] == '%' && format[1] != '\0') {
			switch (var_get_key(++format)) {
			case 'v':
				mask |= UIDL_UIDVALIDITY;
				break;
			case 'u':
				mask |= UIDL_UID;
				break;
			case 'm':
				mask |= UIDL_MD5;
				break;
			case 'f':
				mask |= UIDL_FILE_NAME;
				break;
			}
		}
	}
	return mask;
}

static void open_logfile(void)
{
	const char *user;

	if (getenv("LOG_TO_MASTER") != NULL) {
		i_set_failure_internal();
		return;
	}

	user = getenv("USER");
	if (user == NULL) user = "??";
	if (strlen(user) >= sizeof(log_prefix)-6) {
		/* quite a long user name, cut it */
		user = t_strndup(user, sizeof(log_prefix)-6-2);
		user = t_strconcat(user, "..", NULL);
	}
	i_snprintf(log_prefix, sizeof(log_prefix), "pop3(%s)", user);

	if (getenv("USE_SYSLOG") != NULL) {
		const char *env = getenv("SYSLOG_FACILITY");
		i_set_failure_syslog(log_prefix, LOG_NDELAY,
				     env == NULL ? LOG_MAIL : atoi(env));
	} else {
		/* log to file or stderr */
		i_set_failure_file(getenv("LOGFILE"), log_prefix);
	}

	if (getenv("INFOLOGFILE") != NULL)
		i_set_info_file(getenv("INFOLOGFILE"));

	i_set_failure_timestamp_format(getenv("LOGSTAMP"));
}

static void drop_privileges(void)
{
	/* Log file or syslog opening probably requires roots */
	open_logfile();

	/* Most likely needed. Have to open /dev/urandom before possible
	   chrooting. */
	random_init();

	restrict_access_by_env(!IS_STANDALONE());
}

static int main_init(void)
{
        enum mail_storage_flags flags;
        enum mail_storage_lock_method lock_method;
	struct mail_storage *storage;
	const char *mail;

	lib_signals_init();
        lib_signals_set_handler(SIGINT, TRUE, sig_die, NULL);
        lib_signals_set_handler(SIGTERM, TRUE, sig_die, NULL);
        lib_signals_set_handler(SIGPIPE, FALSE, NULL, NULL);
        lib_signals_set_handler(SIGALRM, FALSE, NULL, NULL);

	if (getenv("USER") == NULL)
		i_fatal("USER environment missing");

	if (getenv("DEBUG") != NULL) {
		i_info("Effective uid=%s, gid=%s",
		       dec2str(geteuid()), dec2str(getegid()));
	}

	if (getenv("STDERR_CLOSE_SHUTDOWN") != NULL) {
		/* If master dies, the log fd gets closed and we'll quit */
		log_io = io_add(STDERR_FILENO, IO_ERROR,
				log_error_callback, NULL);
	}

	dict_client_register();
        mail_storage_init();
	mail_storage_register_all();
	clients_init();

	if (getenv("MODULE_LIST") == NULL)
		modules = NULL;
	else {
		if (getenv("MODULE_DIR") == NULL)
			i_fatal("MODULE_LIST given but MODULE_DIR was not");
		modules = module_dir_load(getenv("MODULE_DIR"),
					  getenv("MODULE_LIST"), TRUE);
	}

	mail = getenv("MAIL");
	if (mail == NULL) {
		/* support also maildir-specific environment */
		mail = getenv("MAILDIR");
		if (mail != NULL)
			mail = t_strconcat("maildir:", mail, NULL);
	}

	parse_workarounds();
	enable_last_command = getenv("POP3_ENABLE_LAST") != NULL;
	no_flag_updates = getenv("POP3_NO_FLAG_UPDATES") != NULL;
	reuse_xuidl = getenv("POP3_REUSE_XUIDL") != NULL;
	lock_session = getenv("POP3_LOCK_SESSION") != NULL;

	uidl_format = getenv("POP3_UIDL_FORMAT");
	if (uidl_format == NULL || *uidl_format == '\0')
		i_fatal("pop3_uidl_format setting is missing from config file");
	logout_format = getenv("POP3_LOGOUT_FORMAT");
	if (logout_format == NULL)
		logout_format = "top=%t/%T, retr=%r/%R, del=%d/%m, size=%s";
	uidl_keymask = parse_uidl_keymask(uidl_format);
	if (uidl_keymask == 0)
		i_fatal("pop3_uidl_format setting doesn't contain any "
			"%% variables.");

	mail_storage_parse_env(&flags, &lock_method);
	storage = mail_storage_create_with_data(mail, getenv("USER"),
						flags, lock_method);
	if (storage == NULL) {
		/* failed */
		if (mail != NULL && *mail != '\0')
			i_fatal("Failed to create storage with data: %s", mail);
		else {
			const char *home;

			home = getenv("HOME");
			if (home == NULL) home = "not set";

			i_fatal("MAIL environment missing and "
				"autodetection failed (home %s)", home);
		}
	}

	if (hook_mail_storage_created != NULL)
		hook_mail_storage_created(storage);

	return client_create(0, 1, storage) != NULL;
}

static void main_deinit(void)
{
	if (log_io != NULL)
		io_remove(&log_io);
	clients_deinit();

	module_dir_unload(&modules);
	mail_storage_deinit();
	dict_client_unregister();
	random_deinit();

	lib_signals_deinit();
	closelog();
}

int main(int argc __attr_unused__, char *argv[], char *envp[])
{
#ifdef DEBUG
	if (getenv("LOGGED_IN") != NULL && getenv("GDB") == NULL)
		fd_debug_verify_leaks(3, 1024);
#endif
	if (IS_STANDALONE() && getuid() == 0 &&
	    net_getpeername(1, NULL, NULL) == 0) {
		printf("-ERR pop3 binary must not be started from "
		       "inetd, use pop3-login instead.\n");
		return 1;
	}

	/* NOTE: we start rooted, so keep the code minimal until
	   restrict_access_by_env() is called */
	lib_init();
	drop_privileges();

        process_title_init(argv, envp);
	ioloop = io_loop_create(system_pool);

	if (main_init())
		io_loop_run(ioloop);
	main_deinit();

	io_loop_destroy(&ioloop);
	lib_deinit();

	return 0;
}
