/* Copyright (C) 2005 Timo Sirainen */

#include "lib.h"
#include "lib-signals.h"
#include "ioloop.h"
#include "fd-close-on-exec.h"
#include "restrict-access.h"
#include "randgen.h"
#include "dict-sql.h"
#include "dict-client.h"
#include "dict-server.h"
#include "module-dir.h"

#include <stdlib.h>
#include <syslog.h>

struct ioloop *ioloop;

static struct module *modules;
static struct dict_server *dict_server;

static void sig_die(int signo, void *context __attr_unused__)
{
	/* warn about being killed because of some signal, except SIGINT (^C)
	   which is too common at least while testing :) */
	if (signo != SIGINT)
		i_warning("Killed with signal %d", signo);
	io_loop_stop(ioloop);
}

static void drop_privileges(void)
{
	/* Log file or syslog opening probably requires roots */
	i_set_failure_internal();

	/* Maybe needed. Have to open /dev/urandom before possible
	   chrooting. */
	random_init();

	restrict_access_by_env(FALSE);
}

static void main_init(void)
{
	lib_signals_init();
        lib_signals_set_handler(SIGINT, TRUE, sig_die, NULL);
        lib_signals_set_handler(SIGTERM, TRUE, sig_die, NULL);
        lib_signals_set_handler(SIGPIPE, FALSE, NULL, NULL);
        lib_signals_set_handler(SIGALRM, FALSE, NULL, NULL);

	dict_client_register();
	dict_sql_register();

	modules = getenv("MODULE_DIR") == NULL ? NULL :
		module_dir_load(getenv("MODULE_DIR"), TRUE);

	dict_server = dict_server_init(DEFAULT_DICT_SERVER_SOCKET_PATH);
}

static void main_deinit(void)
{
	dict_server_deinit(dict_server);

	module_dir_unload(modules);

	dict_sql_unregister();
	dict_client_unregister();

	random_deinit();
	lib_signals_deinit();
	closelog();
}

int main(void)
{
#ifdef DEBUG
	if (getenv("GDB") == NULL)
		fd_debug_verify_leaks(3, 1024);
#endif

	/* NOTE: we start rooted, so keep the code minimal until
	   restrict_access_by_env() is called */
	lib_init();
	drop_privileges();

	ioloop = io_loop_create(system_pool);

	main_init();
        io_loop_run(ioloop);
	main_deinit();

	io_loop_destroy(ioloop);
	lib_deinit();

	return 0;
}
