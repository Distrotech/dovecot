/* Copyright (C) 2002-2003 Timo Sirainen */

#include "lib.h"

#include "ioloop.h"
#include "network.h"
#include "write-full.h"
#include "istream.h"
#include "ostream.h"
#include "process-title.h"
#include "restrict-access.h"

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>

#define MAX_PROXY_INPUT_SIZE 4096
#define OUTBUF_THRESHOLD 1024

#define TIMESTAMP_WAIT_TIME 5
#define TIMESTAMP_FORMAT "* OK [RAWLOG TIMESTAMP] %Y-%m-%d %H:%M:%S\n"

static struct ioloop *ioloop;

struct rawlog_proxy {
	int client_in_fd, client_out_fd, server_fd;
	struct io *client_io, *server_io;
	struct istream *server_input;
	struct ostream *client_output, *server_output;

	int log_in, log_out;

	time_t last_write;
	unsigned int last_out_lf:1;
	unsigned int write_timestamps:1;
};

static void rawlog_proxy_destroy(struct rawlog_proxy *proxy)
{
	if (proxy->log_in != -1) {
		if (close(proxy->log_in) < 0)
			i_error("close(in) failed: %m");
	}
	if (proxy->log_out != -1) {
		if (close(proxy->log_out) < 0)
			i_error("close(out) failed: %m");
	}
	if (proxy->client_io != NULL)
		io_remove(proxy->client_io);
	if (proxy->server_io != NULL)
		io_remove(proxy->server_io);

	i_stream_unref(proxy->server_input);
	o_stream_unref(proxy->client_output);
	o_stream_unref(proxy->server_output);

	if (close(proxy->client_in_fd) < 0)
		i_error("close(client_in_fd) failed: %m");
	if (close(proxy->client_out_fd) < 0)
		i_error("close(client_out_fd) failed: %m");
	if (close(proxy->server_fd) < 0)
		i_error("close(server_fd) failed: %m");
	i_free(proxy);

	io_loop_stop(ioloop);
}

static void proxy_write_in(struct rawlog_proxy *proxy,
			   const void *data, size_t size)
{
	if (proxy->log_out == -1 || size == 0)
		return;

	if (write_full(proxy->log_in, data, size) < 0) {
		/* failed, disable logging */
		i_error("write(in) failed: %m");
		(void)close(proxy->log_in);
		proxy->log_in = -1;
	}
}

static void proxy_write_out(struct rawlog_proxy *proxy,
			    const void *data, size_t size)
{
	struct tm *tm;
	char buf[256];

	if (proxy->log_out == -1 || size == 0)
		return;

	if (proxy->last_out_lf && proxy->write_timestamps &&
	    ioloop_time - proxy->last_write >= TIMESTAMP_WAIT_TIME) {
		tm = localtime(&ioloop_time);

		if (strftime(buf, sizeof(buf), TIMESTAMP_FORMAT, tm) <= 0)
			i_fatal("strftime() failed");
		if (write_full(proxy->log_out, buf, strlen(buf)) < 0)
			i_fatal("Can't write to log file: %m");
	}

	if (write_full(proxy->log_out, data, size) < 0) {
		/* failed, disable logging */
		i_error("write(out) failed: %m");
		(void)close(proxy->log_out);
		proxy->log_out = -1;
	}

	proxy->last_write = ioloop_time;
	proxy->last_out_lf = ((const unsigned char *)buf)[size-1] == '\n';
}

static void server_input(void *context)
{
	struct rawlog_proxy *proxy = context;
	unsigned char buf[OUTBUF_THRESHOLD];
	ssize_t ret;

	if (o_stream_get_buffer_used_size(proxy->client_output) >
	    OUTBUF_THRESHOLD) {
		/* client's output buffer is already quite full.
		   don't send more until we're below threshold. */
		io_remove(proxy->server_io);
		proxy->server_io = NULL;
		return;
	}

	ret = net_receive(proxy->server_fd, buf, sizeof(buf));
	if (ret > 0) {
		(void)o_stream_send(proxy->client_output, buf, ret);
		proxy_write_out(proxy, buf, ret);
	} else if (ret <= 0)
                rawlog_proxy_destroy(proxy);
}

static void client_input(void *context)
{
	struct rawlog_proxy *proxy = context;
	unsigned char buf[OUTBUF_THRESHOLD];
	ssize_t ret;

	if (o_stream_get_buffer_used_size(proxy->server_output) >
	    OUTBUF_THRESHOLD) {
		/* proxy's output buffer is already quite full.
		   don't send more until we're below threshold. */
		io_remove(proxy->client_io);
		proxy->client_io = NULL;
		return;
	}

	ret = net_receive(proxy->client_in_fd, buf, sizeof(buf));
	if (ret > 0) {
		(void)o_stream_send(proxy->server_output, buf, ret);
		proxy_write_in(proxy, buf, ret);
	} else if (ret < 0)
                rawlog_proxy_destroy(proxy);
}

static int server_output(void *context)
{
	struct rawlog_proxy *proxy = context;

	if (o_stream_flush(proxy->server_output) < 0) {
                rawlog_proxy_destroy(proxy);
		return 1;
	}

	if (proxy->client_io == NULL &&
	    o_stream_get_buffer_used_size(proxy->server_output) <
	    OUTBUF_THRESHOLD) {
		/* there's again space in proxy's output buffer, so we can
		   read more from client. */
		proxy->client_io = io_add(proxy->client_in_fd, IO_READ,
					  client_input, proxy);
	}
	return 1;
}

static int client_output(void *context)
{
	struct rawlog_proxy *proxy = context;

	if (o_stream_flush(proxy->client_output) < 0) {
                rawlog_proxy_destroy(proxy);
		return 1;
	}

	if (proxy->server_io == NULL &&
	    o_stream_get_buffer_used_size(proxy->client_output) <
	    OUTBUF_THRESHOLD) {
		/* there's again space in client's output buffer, so we can
		   read more from proxy. */
		proxy->server_io =
			io_add(proxy->server_fd, IO_READ, server_input, proxy);
	}
	return 1;
}

static void proxy_open_logs(struct rawlog_proxy *proxy, const char *path)
{
	time_t now;
	struct tm *tm;
	const char *fname;
	char timestamp[50];

	now = time(NULL);
	tm = localtime(&now);
	if (strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", tm) <= 0)
		i_fatal("strftime() failed");

	fname = t_strdup_printf("%s/%s-%s.in", path, timestamp,
				dec2str(getpid()));
	proxy->log_in = open(fname, O_CREAT|O_EXCL|O_WRONLY, 0600);
	if (proxy->log_in == -1) {
		i_error("rawlog_open: open() failed for %s: %m", fname);
		return;
	}

	fname = t_strdup_printf("%s/%s-%s.out", path, timestamp,
				dec2str(getpid()));
	proxy->log_out = open(fname, O_CREAT|O_EXCL|O_WRONLY, 0600);
	if (proxy->log_out == -1) {
		i_error("rawlog_open: open() failed for %s: %m", fname);
		close(proxy->log_in);
		proxy->log_in = -1;
		return;
	}
}

static struct rawlog_proxy *
rawlog_proxy_create(int client_in_fd, int client_out_fd, int server_fd,
		    const char *path, int write_timestamps)
{
	struct rawlog_proxy *proxy;

	proxy = i_new(struct rawlog_proxy, 1);
	proxy->server_fd = server_fd;
	proxy->server_input =
		i_stream_create_file(server_fd, default_pool,
				     MAX_PROXY_INPUT_SIZE, FALSE);
	proxy->server_output =
		o_stream_create_file(server_fd, default_pool,
				     (size_t)-1, FALSE);
	proxy->server_io = io_add(server_fd, IO_READ, server_input, proxy);
	o_stream_set_flush_callback(proxy->server_output, server_output, proxy);

	proxy->client_in_fd = client_in_fd;
	proxy->client_out_fd = client_out_fd;
	proxy->client_output =
		o_stream_create_file(client_out_fd, default_pool,
				     (size_t)-1, FALSE);
	proxy->client_io = io_add(proxy->client_in_fd, IO_READ,
				  client_input, proxy);
	o_stream_set_flush_callback(proxy->client_output, client_output, proxy);

	proxy->last_out_lf = TRUE;
	proxy->write_timestamps = write_timestamps;

	proxy_open_logs(proxy, path);
	return proxy;
}

static void rawlog_open(int write_timestamps)
{
	const char *home, *path;
	struct stat st;
	int sfd[2];
	pid_t pid;

	home = getenv("HOME");
	if (home == NULL)
		home = ".";

	/* see if we want rawlog */
	path = t_strconcat(home, "/dovecot.rawlog", NULL);
	if (lstat(path, &st) < 0) {
		if (errno != ENOENT)
			i_warning("lstat() failed for %s: %m", path);
		return;
	}
	if (!S_ISDIR(st.st_mode))
		return;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sfd) < 0)
		i_fatal("socketpair() failed: %m");

	pid = fork();
	if (pid < 0)
		i_fatal("fork() failed: %m");

	if (pid > 0) {
		/* parent */
		if (dup2(sfd[1], 0) < 0)
			i_fatal("dup2(sfd, 0)");
		if (dup2(sfd[1], 1) < 0)
			i_fatal("dup2(sfd, 1)");
		close(sfd[0]);
		close(sfd[1]);
		return;
	}
	close(sfd[1]);

	restrict_access_by_env(TRUE);

	process_title_set(t_strdup_printf("[%s:%s rawlog]", getenv("USER"),
					  dec2str(getppid())));

	ioloop = io_loop_create(system_pool);
	rawlog_proxy_create(0, 1, sfd[0], path, write_timestamps);
	io_loop_run(ioloop);
	io_loop_destroy(ioloop);

	lib_deinit();
	exit(0);
}

int main(int argc, char *argv[], char *envp[])
{
	char *executable, *p;

	lib_init();
	process_title_init(argv, envp);

	if (argc < 2)
		i_fatal("Usage: rawlog <binary> <arguments>");

	argv++;
	executable = argv[0];

	rawlog_open(strstr(executable, "/imap") != NULL);

	/* hide path, it's ugly */
	p = strrchr(argv[0], '/');
	if (p != NULL) argv[0] = p+1;
	execv(executable, argv);

	i_fatal_status(FATAL_EXEC, "execv(%s) failed: %m", executable);

	/* not reached */
	return FATAL_EXEC;
}
