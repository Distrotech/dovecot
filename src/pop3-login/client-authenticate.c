/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "base64.h"
#include "buffer.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "safe-memset.h"
#include "str.h"
#include "auth-connection.h"
#include "../auth/auth-mech-desc.h"
#include "../pop3/capability.h"
#include "master.h"
#include "auth-common.h"
#include "client.h"
#include "client-authenticate.h"
#include "ssl-proxy.h"

static enum auth_mech auth_mechs = 0;
static char *auth_mechs_capability = NULL;

int cmd_capa(struct pop3_client *client, const char *args __attr_unused__)
{
	string_t *str;
	int i;

	if (auth_mechs != available_auth_mechs) {
		auth_mechs = available_auth_mechs;
		i_free(auth_mechs_capability);

		str = t_str_new(128);

		str_append(str, "SASL");
		for (i = 0; i < AUTH_MECH_COUNT; i++) {
			if ((auth_mechs & auth_mech_desc[i].mech) &&
			    auth_mech_desc[i].name != NULL &&
			    (client->tls || !auth_mech_desc[i].plaintext ||
			     !disable_plaintext_auth)) {
				str_append_c(str, ' ');
				str_append(str, auth_mech_desc[i].name);
			}
		}

		auth_mechs_capability = i_strdup(str_c(str));
	}

	client_send_line(client, t_strconcat("+OK\r\n" POP3_CAPABILITY_REPLY,
					     (ssl_initialized && !client->tls) ?
					     "STLS\r\n" : "",
					     auth_mechs_capability,
					     "\r\n.", NULL));
	return TRUE;
}

static struct auth_mech_desc *auth_mech_find(const char *name)
{
	int i;

	for (i = 0; i < AUTH_MECH_COUNT; i++) {
		if (auth_mech_desc[i].name != NULL &&
		    strcasecmp(auth_mech_desc[i].name, name) == 0)
			return &auth_mech_desc[i];
	}

	return NULL;
}

static void client_auth_abort(struct pop3_client *client, const char *msg)
{
	if (client->common.auth_request != NULL) {
		auth_abort_request(client->common.auth_request);
		client->common.auth_request = NULL;
	}

	client_send_line(client, msg != NULL ? t_strconcat("-ERR ", msg, NULL) :
			 "-ERR Authentication failed.");
	o_stream_flush(client->output);

	/* get back to normal client input */
	if (client->common.io != NULL)
		io_remove(client->common.io);
	client->common.io = client->common.fd == -1 ? NULL :
		io_add(client->common.fd, IO_READ, client_input, client);

	client_unref(client);
}

static void master_callback(struct client *_client, int success)
{
	struct pop3_client *client = (struct pop3_client *) _client;
	const char *reason = NULL;

	if (success) {
		reason = t_strconcat("Login: ", client->common.virtual_user,
				     NULL);
	} else {
		reason = t_strconcat("Internal login failure: ",
				     client->common.virtual_user, NULL);
		client_send_line(client, "* BYE Internal login failure.");
	}

	client_destroy(client, reason);
	client_unref(client);
}

static void client_send_auth_data(struct pop3_client *client,
				  const unsigned char *data, size_t size)
{
	buffer_t *buf;

	t_push();

	buf = buffer_create_dynamic(data_stack_pool, size*2, (size_t)-1);
	buffer_append(buf, "+ ", 2);
	base64_encode(data, size, buf);
	buffer_append(buf, "\r\n", 2);

	o_stream_send(client->output, buffer_get_data(buf, NULL),
		      buffer_get_used_size(buf));
	o_stream_flush(client->output);

	t_pop();
}

static void login_callback(struct auth_request *request,
			   struct auth_login_reply *reply,
			   const unsigned char *data, struct client *_client)
{
	struct pop3_client *client = (struct pop3_client *) _client;
	const char *error;
	const void *ptr;
	size_t size;

	switch (auth_callback(request, reply, data, _client,
			      master_callback, &error)) {
	case -1:
		/* login failed */
		client_auth_abort(client, error);
		break;

	case 0:
		ptr = buffer_get_data(client->plain_login, &size);
		auth_continue_request(request, ptr, size);

		buffer_set_used_size(client->plain_login, 0);
		break;

	default:
		/* success, we should be able to log in. if we fail, just
		   disconnect the client. */
		client_send_line(client, "+OK Logged in.");
	}
}

int cmd_user(struct pop3_client *client, const char *args)
{
	if (!client->tls && disable_plaintext_auth) {
		client_send_line(client,
				 "-ERR Plaintext authentication disabled.");
		return TRUE;
	}

	/* authorization ID \0 authentication ID \0 pass */
	buffer_set_used_size(client->plain_login, 0);
	buffer_append_c(client->plain_login, '\0');
	buffer_append(client->plain_login, args, strlen(args));

	client_send_line(client, "+OK");
	return TRUE;
}

int cmd_pass(struct pop3_client *client, const char *args)
{
	const char *error;

	if (buffer_get_used_size(client->plain_login) == 0) {
		client_send_line(client, "-ERR No username given.");
		return TRUE;
	}

	buffer_append_c(client->plain_login, '\0');
	buffer_append(client->plain_login, args, strlen(args));

	client_ref(client);
	if (auth_init_request(AUTH_MECH_PLAIN, AUTH_PROTOCOL_IMAP,
			      login_callback, &client->common, &error)) {
		/* don't read any input from client until login is finished */
		if (client->common.io != NULL) {
			io_remove(client->common.io);
			client->common.io = NULL;
		}
		return TRUE;
	} else {
		client_send_line(client,
			t_strconcat("-ERR Login failed: ", error, NULL));
		client_unref(client);
		return TRUE;
	}
}

static void authenticate_callback(struct auth_request *request,
				  struct auth_login_reply *reply,
				  const unsigned char *data,
				  struct client *_client)
{
	struct pop3_client *client = (struct pop3_client *) _client;
	const char *error;

	switch (auth_callback(request, reply, data, _client,
			      master_callback, &error)) {
	case -1:
		/* login failed */
		client_auth_abort(client, error);
		break;

	case 0:
		client_send_auth_data(client, data, reply->data_size);
		break;

	default:
		/* success, we should be able to log in. if we fail, just
		   disconnect the client. */
		client_send_line(client, "+OK Logged in.");
	}
}

static void client_auth_input(void *context)
{
	struct pop3_client *client = context;
	buffer_t *buf;
	char *line;
	size_t linelen, bufsize;

	if (!client_read(client))
		return;

	/* @UNSAFE */
	line = i_stream_next_line(client->input);
	if (line == NULL)
		return;

	if (strcmp(line, "*") == 0) {
		client_auth_abort(client, "Authentication aborted");
		return;
	}

	linelen = strlen(line);
	buf = buffer_create_static_hard(data_stack_pool, linelen);

	if (base64_decode((const unsigned char *) line, linelen,
			  NULL, buf) <= 0) {
		/* failed */
		client_auth_abort(client, "Invalid base64 data");
	} else if (client->common.auth_request == NULL) {
		client_auth_abort(client, "Don't send unrequested data");
	} else {
		auth_continue_request(client->common.auth_request,
				      buffer_get_data(buf, NULL),
				      buffer_get_used_size(buf));
	}

	/* clear sensitive data */
	safe_memset(line, 0, linelen);

	bufsize = buffer_get_used_size(buf);
	safe_memset(buffer_free_without_data(buf), 0, bufsize);
}

int cmd_auth(struct pop3_client *client, const char *args)
{
	struct auth_mech_desc *mech;
	const char *error;

	/* we have only one argument: authentication mechanism name */
	mech = auth_mech_find(args);
	if (mech == NULL) {
		client_send_line(client,
				 "-ERR Unsupported authentication mechanism.");
		return TRUE;
	}

	if (!client->tls && mech->plaintext && disable_plaintext_auth) {
		client_send_line(client,
				 "-ERR Plaintext authentication disabled.");
		return TRUE;
	}

	client_ref(client);
	if (auth_init_request(mech->mech, AUTH_PROTOCOL_IMAP,
			      authenticate_callback, &client->common, &error)) {
		/* following input data will go to authentication */
		if (client->common.io != NULL)
			io_remove(client->common.io);
		client->common.io = io_add(client->common.fd, IO_READ,
					   client_auth_input, client);
	} else {
		client_send_line(client, t_strconcat(
			"-ERR Authentication failed: ", error, NULL));
		client_unref(client);
	}

	return TRUE;
}
