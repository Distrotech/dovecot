/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "ioloop.h"
#include "lib-signals.h"
#include "restrict-access.h"
#include "restrict-process-size.h"
#include "process-title.h"
#include "fd-close-on-exec.h"
#include "master.h"
#include "client-common.h"
#include "auth-client.h"
#include "ssl-proxy.h"

#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>

int disable_plaintext_auth, process_per_connection, verbose_proctitle;
int verbose_ssl;
unsigned int max_logging_users;
unsigned int login_process_uid;
struct auth_client *auth_client;

static struct ioloop *ioloop;
static struct io *io_listen, *io_ssl_listen;
static int main_refcount;
static int is_inetd, closing_down;

void main_ref(void)
{
	main_refcount++;
}

void main_unref(void)
{
	if (--main_refcount == 0) {
		/* nothing to do, quit */
		io_loop_stop(ioloop);
	} else if (closing_down && clients_get_count() == 0) {
		/* last login finished, close all communications
		   to master process */
		master_close();
	}
}

void main_close_listen(void)
{
	if (closing_down)
		return;

	if (io_listen != NULL) {
		if (close(LOGIN_LISTEN_FD) < 0)
			i_fatal("close(listen) failed: %m");

		io_remove(io_listen);
		io_listen = NULL;
	}

	if (io_ssl_listen != NULL) {
		if (close(LOGIN_SSL_LISTEN_FD) < 0)
			i_fatal("close(ssl_listen) failed: %m");

		io_remove(io_ssl_listen);
		io_ssl_listen = NULL;
	}

	closing_down = TRUE;
	master_notify_finished();
}

static void sig_quit(int signo __attr_unused__)
{
	io_loop_stop(ioloop);
}

static void login_accept(void *context __attr_unused__)
{
	struct ip_addr ip, local_ip;
	int fd;

	fd = net_accept(LOGIN_LISTEN_FD, &ip, NULL);
	if (fd < 0) {
		if (fd < -1)
			i_fatal("accept() failed: %m");
		return;
	}

	if (process_per_connection)
		main_close_listen();

	if (net_getsockname(fd, &local_ip, NULL) < 0)
		memset(&local_ip, 0, sizeof(local_ip));

	(void)client_create(fd, FALSE, &local_ip, &ip);
}

static void login_accept_ssl(void *context __attr_unused__)
{
	struct ip_addr ip, local_ip;
	struct client *client;
	struct ssl_proxy *proxy;
	int fd, fd_ssl;

	fd = net_accept(LOGIN_SSL_LISTEN_FD, &ip, NULL);
	if (fd < 0) {
		if (fd < -1)
			i_fatal("accept() failed: %m");
		return;
	}

	if (process_per_connection)
		main_close_listen();
	if (net_getsockname(fd, &local_ip, NULL) < 0)
		memset(&local_ip, 0, sizeof(local_ip));

	fd_ssl = ssl_proxy_new(fd, &ip, &proxy);
	if (fd_ssl == -1)
		net_disconnect(fd);
	else {
		client = client_create(fd_ssl, TRUE, &local_ip, &ip);
		client->proxy = proxy;
	}
}

static void auth_connect_notify(struct auth_client *client __attr_unused__,
				int connected, void *context __attr_unused__)
{
	if (connected)
                clients_notify_auth_connected();
}

static void drop_privileges()
{
	i_set_failure_internal();

	/* Initialize SSL proxy so it can read certificate and private
	   key file. */
	ssl_proxy_init();

	/* Refuse to run as root - we should never need it and it's
	   dangerous with SSL. */
	restrict_access_by_env(TRUE);

	/* make sure we can't fork() */
	restrict_process_size((unsigned int)-1, 1);
}

static void main_init(void)
{
	const char *value;

	lib_init_signals(sig_quit);

	disable_plaintext_auth = getenv("DISABLE_PLAINTEXT_AUTH") != NULL;
	process_per_connection = getenv("PROCESS_PER_CONNECTION") != NULL;
	verbose_proctitle = getenv("VERBOSE_PROCTITLE") != NULL;
        verbose_ssl = getenv("VERBOSE_SSL") != NULL;

	value = getenv("MAX_LOGGING_USERS");
	max_logging_users = value == NULL ? 0 : strtoul(value, NULL, 10);

	value = getenv("PROCESS_UID");
	if (value == NULL)
		i_fatal("BUG: PROCESS_UID environment not given");
        login_process_uid = strtoul(value, NULL, 10);
	if (login_process_uid == 0)
		i_fatal("BUG: PROCESS_UID environment is 0");

        closing_down = FALSE;
	main_refcount = 0;

	auth_client = auth_client_new((unsigned int)getpid());
        auth_client_set_connect_notify(auth_client, auth_connect_notify, NULL);
	clients_init();

	io_listen = io_ssl_listen = NULL;

	if (!is_inetd) {
		if (net_getsockname(LOGIN_LISTEN_FD, NULL, NULL) == 0) {
			io_listen = io_add(LOGIN_LISTEN_FD, IO_READ,
					   login_accept, NULL);
		}

		if (net_getsockname(LOGIN_SSL_LISTEN_FD, NULL, NULL) == 0) {
			if (!ssl_initialized) {
				/* this shouldn't happen, master should have
				   disabled the ssl socket.. */
				i_fatal("BUG: SSL initialization parameters "
					"not given while they should have "
					"been");
			}

			io_ssl_listen = io_add(LOGIN_SSL_LISTEN_FD, IO_READ,
					       login_accept_ssl, NULL);
		}

		/* initialize master last - it sends the "we're ok"
		   notification */
		master_init(LOGIN_MASTER_SOCKET_FD, TRUE);
	}
}

static void main_deinit(void)
{
        if (lib_signal_kill != 0)
		i_warning("Killed with signal %d", lib_signal_kill);

	if (io_listen != NULL) io_remove(io_listen);
	if (io_ssl_listen != NULL) io_remove(io_ssl_listen);

	ssl_proxy_deinit();

	auth_client_free(auth_client);
	clients_deinit();
	master_deinit();

	closelog();
}

int main(int argc __attr_unused__, char *argv[], char *envp[])
{
	const char *name, *group_name;
	struct ip_addr ip, local_ip;
	struct ssl_proxy *proxy = NULL;
	struct client *client;
	int i, fd = -1, master_fd = -1;

	is_inetd = getenv("DOVECOT_MASTER") == NULL;

#ifdef DEBUG
	if (!is_inetd && getenv("GDB") == NULL)
		fd_debug_verify_leaks(4, 1024);
#endif
	/* NOTE: we start rooted, so keep the code minimal until
	   restrict_access_by_env() is called */
	lib_init();

	if (is_inetd) {
		/* running from inetd. create master process before
		   dropping privileges. */
		group_name = strrchr(argv[0], '/');
		group_name = group_name == NULL ? argv[0] : group_name+1;
		group_name = t_strcut(group_name, '-');

		for (i = 1; i < argc; i++) {
			if (strncmp(argv[i], "--group=", 8) == 0) {
				group_name = argv[1]+8;
				break;
			}
		}

		master_fd = master_connect(group_name);
	}

	name = strrchr(argv[0], '/');
	drop_privileges();

	process_title_init(argv, envp);
	ioloop = io_loop_create(system_pool);
	main_init();

	if (is_inetd) {
		if (net_getpeername(1, &ip, NULL) < 0) {
			i_fatal("%s can be started only through dovecot "
				"master process, inetd or equilevant", argv[0]);
		}
		if (net_getsockname(1, &local_ip, NULL) < 0)
			memset(&local_ip, 0, sizeof(local_ip));

		fd = 1;
		for (i = 1; i < argc; i++) {
			if (strcmp(argv[i], "--ssl") == 0) {
				fd = ssl_proxy_new(fd, &ip, &proxy);
				if (fd == -1)
					i_fatal("SSL initialization failed");
			} else if (strncmp(argv[i], "--group=", 8) != 0)
				i_fatal("Unknown parameter: %s", argv[i]);
		}

		master_init(master_fd, FALSE);
		closing_down = TRUE;

		if (fd != -1) {
			client = client_create(fd, TRUE, &local_ip, &ip);
			client->proxy = proxy;
		}
	}

	io_loop_run(ioloop);
	main_deinit();

	io_loop_destroy(ioloop);
	lib_deinit();

        return 0;
}
