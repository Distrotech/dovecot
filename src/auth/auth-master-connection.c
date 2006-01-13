/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "array.h"
#include "buffer.h"
#include "hash.h"
#include "str.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "network.h"
#include "userdb.h"
#include "auth-request-handler.h"
#include "auth-master-interface.h"
#include "auth-client-connection.h"
#include "auth-master-listener.h"
#include "auth-master-connection.h"

#include <unistd.h>
#include <stdlib.h>

#define MAX_INBUF_SIZE 1024
#define MAX_OUTBUF_SIZE (1024*50)

struct master_userdb_request {
	struct auth_master_connection *conn;
	unsigned int id;
	struct auth_request *auth_request;
};

static int master_output(void *context);

void auth_master_request_callback(const char *reply, void *context)
{
	struct auth_master_connection *conn = context;
	struct const_iovec iov[2];

	if (conn->listener->auth->verbose_debug)
		i_info("master out: %s", reply);

	iov[0].iov_base = reply;
	iov[0].iov_len = strlen(reply);
	iov[1].iov_base = "\n";
	iov[1].iov_len = 1;

	(void)o_stream_sendv(conn->output, iov, 2);
}

static bool
master_input_request(struct auth_master_connection *conn, const char *args)
{
	struct auth_client_connection *client_conn;
	const char *const *list;
	unsigned int id, client_pid, client_id;

	/* <id> <client-pid> <client-id> */
	list = t_strsplit(args, "\t");
	if (list[0] == NULL || list[1] == NULL || list[2] == NULL) {
		i_error("BUG: Master sent broken REQUEST");
		return FALSE;
	}

	id = (unsigned int)strtoul(list[0], NULL, 10);
	client_pid = (unsigned int)strtoul(list[1], NULL, 10);
	client_id = (unsigned int)strtoul(list[2], NULL, 10);

	client_conn = auth_client_connection_lookup(conn->listener, client_pid);
	if (client_conn == NULL) {
		i_error("Master requested auth for nonexisting client %u",
			client_pid);
		(void)o_stream_send_str(conn->output,
					t_strdup_printf("NOTFOUND\t%u\n", id));
	} else {
		auth_request_handler_master_request(
			client_conn->request_handler, conn, id, client_id);
	}
	return TRUE;
}

static void
user_callback(struct auth_stream_reply *reply,
	      struct auth_request *auth_request)
{
	struct auth_master_connection *conn = auth_request->context;
	string_t *str;

	str = t_str_new(128);
	if (reply == NULL)
		str_printfa(str, "NOTFOUND\t%u\n", auth_request->id);
	else {
		str_printfa(str, "USER\t%u\t", auth_request->id);
		str_append(str, auth_stream_reply_export(reply));
		str_append_c(str, '\n');
	}

	if (conn->listener->auth->verbose_debug)
		i_info("master out: %s", str_c(str));

	(void)o_stream_send(conn->output, str_data(str), str_len(str));
	auth_request_unref(auth_request);
}

static bool
master_input_user(struct auth_master_connection *conn, const char *args)
{
	struct auth_request *auth_request;
	const char *const *list, *name, *arg;

	/* <id> <userid> [<parameters>] */
	list = t_strsplit(args, "\t");
	if (list[0] == NULL || list[1] == NULL) {
		i_error("BUG: Master sent broken USER");
		return FALSE;
	}

	auth_request = auth_request_new_dummy(conn->listener->auth);
	auth_request->id = (unsigned int)strtoul(list[0], NULL, 10);
	auth_request->user = p_strdup(auth_request->pool, list[1]);
	auth_request->context = conn;

	for (list += 2; *list != NULL; list++) {
		arg = strchr(*list, '=');
		if (arg == NULL) {
			name = *list;
			arg = "";
		} else {
			name = t_strdup_until(*list, arg);
			arg++;
		}

		(void)auth_request_import(auth_request, name, arg);
	}

	if (auth_request->service == NULL) {
		i_error("BUG: Master sent USER request without service");
		auth_request_unref(auth_request);
		return FALSE;
	}

	auth_request_lookup_user(auth_request, user_callback);
	return TRUE;
}

static bool
master_input_die(struct auth_master_connection *conn)
{
	return TRUE;
}

static void master_input(void *context)
{
	struct auth_master_connection *conn = context;
 	char *line;
	bool ret;

	switch (i_stream_read(conn->input)) {
	case 0:
		return;
	case -1:
		/* disconnected */
                auth_master_connection_destroy(conn);
		return;
	case -2:
		/* buffer full */
		i_error("BUG: Master sent us more than %d bytes",
			(int)MAX_INBUF_SIZE);
                auth_master_connection_destroy(conn);
		return;
	}

	if (!conn->version_received) {
		line = i_stream_next_line(conn->input);
		if (line == NULL)
			return;

		/* make sure the major version matches */
		if (strncmp(line, "VERSION\t", 8) != 0 ||
		    atoi(t_strcut(line + 8, '\t')) !=
		    AUTH_MASTER_PROTOCOL_MAJOR_VERSION) {
			i_error("Master not compatible with this server "
				"(mixed old and new binaries?)");
			auth_master_connection_destroy(conn);
			return;
		}
		conn->version_received = TRUE;
	}

	while ((line = i_stream_next_line(conn->input)) != NULL) {
		if (conn->listener->auth->verbose_debug)
			i_info("master in: %s", line);

		t_push();
		if (strncmp(line, "REQUEST\t", 8) == 0)
			ret = master_input_request(conn, line + 8);
		else if (strncmp(line, "USER\t", 5) == 0)
			ret = master_input_user(conn, line + 5);
		else if (strcmp(line, "DIE") == 0)
			ret = master_input_die(conn);
		else {
			/* ignore unknown command */
			ret = TRUE;
		}
		t_pop();

		if (!ret) {
			auth_master_connection_destroy(conn);
			return;
		}
	}
}

static int master_output(void *context)
{
	struct auth_master_connection *conn = context;
	int ret;

	if ((ret = o_stream_flush(conn->output)) < 0) {
		/* transmit error, probably master died */
		auth_master_connection_destroy(conn);
		return 1;
	}

	if (o_stream_get_buffer_used_size(conn->output) <= MAX_OUTBUF_SIZE/2) {
		/* allow input again */
		conn->io = io_add(conn->fd, IO_READ, master_input, conn);
	}
	return 1;
}

struct auth_master_connection *
auth_master_connection_create(struct auth_master_listener *listener, int fd)
{
	struct auth_master_connection *conn;

	conn = i_new(struct auth_master_connection, 1);
	conn->listener = listener;
	conn->fd = fd;
	conn->input = i_stream_create_file(fd, default_pool,
					   MAX_INBUF_SIZE, FALSE);
	conn->output = o_stream_create_file(fd, default_pool,
					    (size_t)-1, FALSE);
	o_stream_set_flush_callback(conn->output, master_output, conn);
	conn->io = io_add(fd, IO_READ, master_input, conn);

	array_append(&listener->masters, &conn, 1);
	return conn;
}

void auth_master_connection_send_handshake(struct auth_master_connection *conn)
{
	const char *line;

	if (conn->output == NULL)
		return;

	line = t_strdup_printf("VERSION\t%u\t%u\nSPID\t%u\n",
			       AUTH_MASTER_PROTOCOL_MAJOR_VERSION,
			       AUTH_MASTER_PROTOCOL_MINOR_VERSION,
			       conn->listener->pid);
	(void)o_stream_send_str(conn->output, line);
}

void auth_master_connection_destroy(struct auth_master_connection *conn)
{
        struct auth_master_connection *const *conns;
	unsigned int i, count;

	if (conn->destroyed)
		return;
	conn->destroyed = TRUE;

	if (conn->fd != -1) {
		if (close(conn->fd) < 0)
			i_error("close(): %m");
	}
	if (conn->input != NULL)
		i_stream_unref(conn->input);
	if (conn->output != NULL)
		o_stream_unref(conn->output);
	if (conn->io != NULL)
		io_remove(conn->io);

	conns = array_get(&conn->listener->masters, &count);
	for (i = 0; i < count; i++) {
		if (conns[i] == conn) {
			array_delete(&conn->listener->masters, i, 1);
			break;
		}
	}
	if (!standalone && auth_master_listeners_masters_left() == 0)
		io_loop_stop(ioloop);
       
	i_free(conn);
}
