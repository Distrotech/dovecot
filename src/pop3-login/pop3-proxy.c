/* Copyright (C) 2004 Timo Sirainen */

#include "common.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "base64.h"
#include "safe-memset.h"
#include "str.h"
#include "client.h"
#include "pop3-proxy.h"

static void proxy_input(struct istream *input, struct ostream *output,
			void *context)
{
	struct pop3_client *client = context;
	string_t *str;
	const char *line;

	if (input == NULL) {
		if (client->io != NULL) {
			/* remote authentication failed, we're just
			   freeing the proxy */
			return;
		}

		/* failed for some reason */
		client_destroy_internal_failure(client);
		return;
	}

	switch (i_stream_read(input)) {
	case -2:
		/* buffer full */
		i_error("pop-proxy(%s): Remote input buffer full",
			client->common.virtual_user);
		client_destroy_internal_failure(client);
		return;
	case -1:
		/* disconnected */
		client_destroy(client, "Proxy: Remote disconnected");
		return;
	}

	line = i_stream_next_line(input);
	if (line == NULL)
		return;

	switch (client->proxy_state) {
	case 0:
		/* this is a banner */
		if (strncmp(line, "+OK", 3) != 0) {
			i_error("pop3-proxy(%s): "
				"Remote returned invalid banner: %s",
				client->common.virtual_user, line);
			client_destroy_internal_failure(client);
			return;
		}

		/* send USER command */
		str = t_str_new(128);
		str_append(str, "USER ");
		str_append(str, client->proxy_user);
		str_append(str, "\r\n");
		(void)o_stream_send(output, str_data(str), str_len(str));

		client->proxy_state++;
		return;
	case 1:
		if (strncmp(line, "+OK", 3) != 0)
			break;

		/* USER successful, send PASS */
		str = t_str_new(128);
		str_append(str, "PASS ");
		str_append(str, client->proxy_password);
		str_append(str, "\r\n");
		(void)o_stream_send(output, str_data(str),
				    str_len(str));

		safe_memset(client->proxy_password, 0,
			    strlen(client->proxy_password));
		i_free(client->proxy_password);
		client->proxy_password = NULL;

		client->proxy_state++;
		return;
	case 2:
		/* Login successful. Send this line to client. */
		(void)o_stream_send_str(client->output, line);
		(void)o_stream_send(client->output, "\r\n", 2);

		login_proxy_detach(client->proxy, client->input,
				   client->output);

		client->proxy = NULL;
		client->input = NULL;
		client->output = NULL;
		client->common.fd = -1;
		client_destroy(client,
			       t_strdup_printf("proxy(%s): started",
					       client->common.virtual_user));
		return;
	}

	/* Login failed. Send our own failure reply so client can't
	   figure out if user exists or not just by looking at the
	   reply string. */
	client_send_line(client, "-ERR "AUTH_FAILED_MSG);

	/* allow client input again */
	i_assert(client->io == NULL);
	client->io = io_add(client->common.fd, IO_READ,
			    client_input, client);

	login_proxy_free(client->proxy);
	client->proxy = NULL;

	if (client->proxy_password != NULL) {
		safe_memset(client->proxy_password, 0,
			    strlen(client->proxy_password));
		i_free(client->proxy_password);
		client->proxy_password = NULL;
	}

	i_free(client->proxy_user);
	client->proxy_user = NULL;
}

int pop3_proxy_new(struct pop3_client *client, const char *host,
		   unsigned int port, const char *user, const char *password)
{
	i_assert(user != NULL);

	if (password == NULL) {
		i_error("proxy(%s): password not given",
			client->common.virtual_user);
		return -1;
	}

	client->proxy = login_proxy_new(&client->common, host, port,
					proxy_input, client);
	if (client->proxy == NULL)
		return -1;

	client->proxy_state = 0;
	client->proxy_user = i_strdup(user);
	client->proxy_password = i_strdup(password);

	/* disable input until authentication is finished */
	if (client->io != NULL) {
		io_remove(client->io);
		client->io = NULL;
	}

	return 0;
}
