/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "buffer.h"
#include "ioloop.h"
#include "network.h"
#include "istream.h"
#include "ostream.h"
#include "str.h"
#include "var-expand.h"
#include "mail-storage.h"
#include "commands.h"
#include "mail-search.h"

#include <stdlib.h>

/* max. length of input command line (spec says 512) */
#define MAX_INBUF_SIZE 2048

/* Stop reading input when output buffer has this many bytes. Once the buffer
   size has dropped to half of it, start reading input again. */
#define OUTBUF_THROTTLE_SIZE 4096

/* If we can't send anything for 10 minutes, disconnect the client */
#define CLIENT_OUTPUT_TIMEOUT (10*60)

/* Disconnect client when it sends too many bad commands in a row */
#define CLIENT_MAX_BAD_COMMANDS 20

/* Disconnect client after idling this many seconds */
#define CLIENT_IDLE_TIMEOUT (10*60)

extern struct mail_storage_callbacks mail_storage_callbacks;

static struct client *my_client; /* we don't need more than one currently */
static struct timeout *to_idle;

static void client_input(void *context);
static int client_output(void *context);

static int sync_mailbox(struct mailbox *box, struct mailbox_status *status)
{
	struct mailbox_sync_context *ctx;
        struct mailbox_sync_rec sync_rec;

	ctx = mailbox_sync_init(box, MAILBOX_SYNC_FLAG_FULL_READ);
	while (mailbox_sync_next(ctx, &sync_rec) > 0)
		;
	return mailbox_sync_deinit(ctx, status);
}

static int init_mailbox(struct client *client)
{
	struct mail_search_arg search_arg;
        struct mailbox_transaction_context *t;
	struct mail_search_context *ctx;
        struct mailbox_status status;
	struct mail *mail;
	buffer_t *message_sizes_buf;
	int i, failed;

	message_sizes_buf = buffer_create_dynamic(default_pool, 512);

	memset(&search_arg, 0, sizeof(search_arg));
	search_arg.type = SEARCH_ALL;

	for (i = 0; i < 2; i++) {
		if (sync_mailbox(client->mailbox, &status) < 0) {
			client_send_storage_error(client);
			break;
		}
		client->uid_validity = status.uidvalidity;

		t = mailbox_transaction_begin(client->mailbox, 0);
		ctx = mailbox_search_init(t, NULL, &search_arg, NULL);
		if (ctx == NULL) {
			client_send_storage_error(client);
			mailbox_transaction_rollback(t);
			break;
		}

		client->last_seen = 0;
		client->total_size = 0;
		buffer_set_used_size(message_sizes_buf, 0);

		failed = FALSE;
		mail = mail_alloc(t, MAIL_FETCH_VIRTUAL_SIZE, NULL);
		while (mailbox_search_next(ctx, mail) > 0) {
			uoff_t size = mail_get_virtual_size(mail);

			if (size == (uoff_t)-1) {
				failed = TRUE;
				break;
			}

			if ((mail_get_flags(mail) & MAIL_SEEN) != 0)
				client->last_seen = mail->seq;
                        client->total_size += size;

			buffer_append(message_sizes_buf, &size, sizeof(size));
		}
		client->messages_count =
			message_sizes_buf->used / sizeof(uoff_t);

		mail_free(mail);
		if (mailbox_search_deinit(ctx) < 0) {
			client_send_storage_error(client);
			mailbox_transaction_rollback(t);
			break;
		}

		if (!failed) {
			client->trans = t;
			client->message_sizes =
				buffer_free_without_data(message_sizes_buf);
			return TRUE;
		}

		/* well, sync and try again */
		mailbox_transaction_rollback(t);
	}

	if (i == 2)
		client_send_line(client, "-ERR [IN-USE] Couldn't sync mailbox.");
	buffer_free(message_sizes_buf);
	return FALSE;
}

struct client *client_create(int hin, int hout, struct mail_storage *storage)
{
	struct client *client;
        enum mailbox_open_flags flags;
	int syntax_error;

	/* always use nonblocking I/O */
	net_set_nonblock(hin, TRUE);
	net_set_nonblock(hout, TRUE);

	client = i_new(struct client, 1);
	client->input = i_stream_create_file(hin, default_pool,
					     MAX_INBUF_SIZE, FALSE);
	client->output = o_stream_create_file(hout, default_pool,
					      (size_t)-1, FALSE);
	o_stream_set_flush_callback(client->output, client_output, client);

	client->io = io_add(hin, IO_READ, client_input, client);
        client->last_input = ioloop_time;
	client->storage = storage;

	mail_storage_set_callbacks(storage, &mail_storage_callbacks, client);

	flags = 0;
	if (no_flag_updates)
		flags |= MAILBOX_OPEN_KEEP_RECENT;
	client->mailbox = mailbox_open(storage, "INBOX", NULL, flags);
	if (client->mailbox == NULL) {
		i_error("Couldn't open INBOX: %s",
			mail_storage_get_last_error(storage, &syntax_error));
		client_send_line(client, "-ERR No INBOX for user.");
		client_destroy(client, "No INBOX for user.");
		return NULL;
	}

	if (!init_mailbox(client)) {
		client_destroy(client, "Mailbox init failed");
		return NULL;
	}

	i_assert(my_client == NULL);
	my_client = client;

	if (hook_client_created != NULL)
		hook_client_created(&client);
	return client;
}

static const char *client_stats(struct client *client)
{
	static struct var_expand_table static_tab[] = {
		{ 'T', NULL },
		{ 't', NULL },
		{ 'R', NULL },
		{ 'r', NULL },
		{ 'd', NULL },
		{ 'm', NULL },
		{ 's', NULL },
		{ '\0', NULL }
	};
	struct var_expand_table *tab;
	string_t *str;

	tab = t_malloc(sizeof(static_tab));
	memcpy(tab, static_tab, sizeof(static_tab));

	tab[0].value = dec2str(client->top_bytes);
	tab[1].value = dec2str(client->top_count);
	tab[2].value = dec2str(client->retr_bytes);
	tab[3].value = dec2str(client->retr_count);
	tab[4].value = dec2str(client->deleted_count);
	tab[5].value = dec2str(client->messages_count);
	tab[6].value = dec2str(client->total_size);

	str = t_str_new(128);
	var_expand(str, logout_format, tab);
	return str_c(str);
}

void client_destroy(struct client *client, const char *reason)
{
	if (reason != NULL)
		i_info("%s %s", reason, client_stats(client));

	if (client->cmd != NULL) {
		/* deinitialize command */
		i_stream_close(client->input);
		o_stream_close(client->output);
		client->cmd(client);
		i_assert(client->cmd == NULL);
	}
	if (client->trans != NULL)
		mailbox_transaction_rollback(client->trans);
	if (client->mailbox != NULL)
		mailbox_close(client->mailbox);
	mail_storage_destroy(client->storage);

	i_free(client->message_sizes);
	i_free(client->deleted_bitmask);

	if (client->io != NULL)
		io_remove(client->io);

	i_stream_unref(client->input);
	o_stream_unref(client->output);

	i_free(client);

	/* quit the program */
	my_client = NULL;
	io_loop_stop(ioloop);
}

void client_disconnect(struct client *client, const char *reason)
{
	if (reason != NULL)
		i_info("%s %s", reason, client_stats(client));

	(void)o_stream_flush(client->output);

	i_stream_close(client->input);
	o_stream_close(client->output);
}

int client_send_line(struct client *client, const char *fmt, ...)
{
	va_list va;
	string_t *str;
	ssize_t ret;

	if (client->output->closed)
		return -1;

	t_push();
	va_start(va, fmt);

	str = t_str_new(256);
	str_vprintfa(str, fmt, va);
	str_append(str, "\r\n");

	ret = o_stream_send(client->output, str_data(str), str_len(str));
	if (ret >= 0) {
		i_assert((size_t)ret == str_len(str));

		if (o_stream_get_buffer_used_size(client->output) <
		    OUTBUF_THROTTLE_SIZE) {
			ret = 1;
			client->last_output = ioloop_time;
		} else {
			ret = 0;
			if (client->io != NULL) {
				/* no more input until client has read
				   our output */
				io_remove(client->io);
				client->io = NULL;

				/* If someone happens to flush output,
				   we want to get our IO handler back in
				   flush callback */
				o_stream_set_flush_pending(client->output,
							   TRUE);
			}
		}
	}

	va_end(va);
	t_pop();
	return (int)ret;
}

void client_send_storage_error(struct client *client)
{
	const char *error;
	int syntax;

	if (mailbox_is_inconsistent(client->mailbox)) {
		client_send_line(client, "-ERR Mailbox is in inconsistent "
				 "state, please relogin.");
		client_disconnect(client, "Mailbox is in inconsistent state.");
		return;
	}

	error = mail_storage_get_last_error(client->storage, &syntax);
	client_send_line(client, "-ERR %s", error != NULL ? error :
			 "BUG: Unknown error");
}

static void client_input(void *context)
{
	struct client *client = context;
	char *line, *args;

	if (client->cmd != NULL) {
		/* we're still processing a command. wait until it's
		   finished. */
		io_remove(client->io);
		client->io = NULL;
		client->waiting_input = TRUE;
		return;
	}

	client->waiting_input = FALSE;
	client->last_input = ioloop_time;

	switch (i_stream_read(client->input)) {
	case -1:
		/* disconnected */
		client_destroy(client, "Disconnected");
		return;
	case -2:
		/* line too long, kill it */
		client_send_line(client, "-ERR Input line too long.");
		client_destroy(client, "Input line too long.");
		return;
	}

	o_stream_cork(client->output);
	while (!client->output->closed &&
	       (line = i_stream_next_line(client->input)) != NULL) {
		args = strchr(line, ' ');
		if (args == NULL)
			args = "";
		else
			*args++ = '\0';

		if (client_command_execute(client, line, args)) {
			client->bad_counter = 0;
			if (client->cmd != NULL) {
				o_stream_set_flush_pending(client->output,
							   TRUE);
				client->waiting_input = TRUE;
				break;
			}
		} else if (++client->bad_counter > CLIENT_MAX_BAD_COMMANDS) {
			client_send_line(client, "-ERR Too many bad commands.");
			client_disconnect(client, "Too many bad commands.");
		}
	}
	o_stream_uncork(client->output);

	if (client->output->closed)
		client_destroy(client, NULL);
}

static int client_output(void *context)
{
	struct client *client = context;
	int ret;

	if ((ret = o_stream_flush(client->output)) < 0) {
		client_destroy(client, NULL);
		return 1;
	}

	client->last_output = ioloop_time;

	if (client->cmd != NULL) {
		o_stream_cork(client->output);
		client->cmd(client);
		o_stream_uncork(client->output);
	}

	if (client->cmd == NULL) {
		if (o_stream_get_buffer_used_size(client->output) <
		    OUTBUF_THROTTLE_SIZE/2 && client->io == NULL) {
			/* enable input again */
			client->io = io_add(i_stream_get_fd(client->input),
					    IO_READ, client_input, client);
		}
		if (client->io != NULL && client->waiting_input)
			client_input(client);
	}

	return client->cmd == NULL;
}

static void idle_timeout(void *context __attr_unused__)
{
	if (my_client == NULL)
		return;

	if (my_client->cmd != NULL) {
		if (ioloop_time - my_client->last_output >=
		    CLIENT_OUTPUT_TIMEOUT)
			client_destroy(my_client, "Disconnected for inactivity.");
	} else {
		if (ioloop_time - my_client->last_input >=
		    CLIENT_IDLE_TIMEOUT) {
			client_send_line(my_client,
					 "-ERR Disconnected for inactivity.");
			client_destroy(my_client, "Disconnected for inactivity.");
		}
	}
}

void clients_init(void)
{
	my_client = NULL;
	to_idle = timeout_add(10000, idle_timeout, NULL);
}

void clients_deinit(void)
{
	if (my_client != NULL) {
		client_send_line(my_client, "-ERR Server shutting down.");
		client_destroy(my_client, "Server shutting down.");
	}

	timeout_remove(to_idle);
}
