/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "ioloop.h"
#include "istream.h"
#include "ostream.h"
#include "commands.h"
#include "imap-sync.h"

#include <stdlib.h>

#define DEFAULT_IDLE_CHECK_INTERVAL 30
/* Send some noice to client every few minutes to avoid NATs and stateful
   firewalls from closing the connection */
#define KEEPALIVE_TIMEOUT (2*60)

struct cmd_idle_context {
	struct client *client;
	struct client_command_context *cmd;

	struct imap_sync_context *sync_ctx;
	struct timeout *idle_to, *keepalive_to;
	uint32_t dummy_seq;

	unsigned int manual_cork:1;
	unsigned int idle_timeout:1;
	unsigned int sync_pending:1;
};

static bool cmd_idle_continue(struct client_command_context *cmd);

static void idle_finish(struct cmd_idle_context *ctx, bool done_ok)
{
	struct client *client = ctx->client;

	if (ctx->idle_to != NULL)
		timeout_remove(&ctx->idle_to);
	if (ctx->keepalive_to != NULL)
		timeout_remove(&ctx->keepalive_to);

	if (ctx->sync_ctx != NULL) {
		/* we're here only in connection failure cases */
		(void)imap_sync_deinit(ctx->sync_ctx);
	}

	o_stream_cork(client->output);

	if (ctx->dummy_seq != 0) {
		/* outlook idle workaround */
		client_send_line(client,
			t_strdup_printf("* %u EXPUNGE", ctx->dummy_seq));
	}

	io_remove(&client->io);

	if (client->mailbox != NULL)
		mailbox_notify_changes(client->mailbox, 0, NULL, NULL);

	if (done_ok)
		client_send_tagline(ctx->cmd, "OK Idle completed.");
	else
		client_send_tagline(ctx->cmd, "BAD Expected DONE.");

	o_stream_uncork(client->output);

	client->bad_counter = 0;
	_client_reset_command(client);

	if (client->input_pending)
		_client_input(client);
}

static void idle_client_input(void *context)
{
        struct cmd_idle_context *ctx = context;
	struct client *client = ctx->client;
	char *line;

	client->last_input = ioloop_time;

	switch (i_stream_read(client->input)) {
	case -1:
		/* disconnected */
		client_destroy(client, "Disconnected in IDLE");
		return;
	case -2:
		client->input_skip_line = TRUE;
		idle_finish(ctx, FALSE);
		return;
	}

	if (ctx->sync_ctx != NULL) {
		/* we're still sending output to client. wait until it's all
		   sent so we don't lose any changes. */
		io_remove(&client->io);
		return;
	}

	while ((line = i_stream_next_line(client->input)) != NULL) {
		if (client->input_skip_line)
			client->input_skip_line = FALSE;
		else {
			idle_finish(ctx, strcmp(line, "DONE") == 0);
			break;
		}
	}
}

static void idle_send_fake_exists(struct cmd_idle_context *ctx)
{
	struct client *client = ctx->client;

	ctx->dummy_seq = client->messages_count+1;
	client_send_line(client,
			 t_strdup_printf("* %u EXISTS", ctx->dummy_seq));
	mailbox_notify_changes(client->mailbox, 0, NULL, NULL);
}

static void idle_timeout(void *context)
{
	struct cmd_idle_context *ctx = context;

	/* outlook workaround - it hasn't sent anything for a long time and
	   we're about to disconnect unless it does something. send a fake
	   EXISTS to see if it responds. it's expunged later. */

	timeout_remove(&ctx->idle_to);

	if (ctx->sync_ctx != NULL) {
		/* we're already syncing.. do this after it's finished */
		ctx->idle_timeout = TRUE;
		return;
	}

	idle_send_fake_exists(ctx);
}

static void keepalive_timeout(void *context)
{
	struct cmd_idle_context *ctx = context;

	if (ctx->client->output_pending) {
		/* it's busy sending output */
		return;
	}

	client_send_line(ctx->client, "* OK Still here");
}

static void idle_sync_now(struct mailbox *box, struct cmd_idle_context *ctx)
{
	i_assert(ctx->sync_ctx == NULL);

	ctx->sync_pending = FALSE;
	ctx->sync_ctx = imap_sync_init(ctx->client, box, 0, 0);
	cmd_idle_continue(ctx->cmd);
}

static void idle_callback(struct mailbox *box, void *context)
{
        struct cmd_idle_context *ctx = context;

	if (ctx->sync_ctx != NULL)
		ctx->sync_pending = TRUE;
	else {
		ctx->manual_cork = TRUE;
		idle_sync_now(box, ctx);
	}
}

static bool cmd_idle_continue(struct client_command_context *cmd)
{
	struct client *client = cmd->client;
	struct cmd_idle_context *ctx = cmd->context;

	if (ctx->manual_cork)  {
		/* we're coming from idle_callback instead of a normal
		   I/O handler, so we'll have to do corking manually */
		o_stream_cork(client->output);
	}

	if (ctx->sync_ctx != NULL) {
		if (imap_sync_more(ctx->sync_ctx) == 0) {
			/* unfinished */
			if (ctx->manual_cork) {
				ctx->manual_cork = FALSE;
				o_stream_uncork(client->output);
			}
			return FALSE;
		}

		if (imap_sync_deinit(ctx->sync_ctx) < 0) {
			client_send_untagged_storage_error(client,
				mailbox_get_storage(client->mailbox));
			mailbox_notify_changes(client->mailbox, 0, NULL, NULL);
		}
		ctx->sync_ctx = NULL;
	}

	if (ctx->idle_timeout) {
		/* outlook workaround */
		idle_send_fake_exists(ctx);
	} else if (ctx->sync_pending) {
		/* more changes occurred while we were sending changes to
		   client */
		idle_sync_now(client->mailbox, ctx);
		/* NOTE: this recurses back to this function,
		   so we return here instead of doing everything twice. */
		return FALSE;
	}
        client->output_pending = FALSE;

	if (ctx->manual_cork) {
		ctx->manual_cork = FALSE;
		o_stream_uncork(client->output);
	}

	if (client->output->closed) {
		idle_finish(ctx, FALSE);
		return TRUE;
	}
	if (client->io == NULL) {
		/* input is pending */
		client->io = io_add(i_stream_get_fd(client->input),
				    IO_READ, idle_client_input, ctx);
		idle_client_input(ctx);
	}
	return FALSE;
}

bool cmd_idle(struct client_command_context *cmd)
{
	struct client *client = cmd->client;
	struct cmd_idle_context *ctx;
	const char *str;
	unsigned int interval;

	ctx = p_new(cmd->pool, struct cmd_idle_context, 1);
	ctx->cmd = cmd;
	ctx->client = client;

	if ((client_workarounds & WORKAROUND_OUTLOOK_IDLE) != 0 &&
	    client->mailbox != NULL) {
		ctx->idle_to = timeout_add((CLIENT_IDLE_TIMEOUT - 60) * 1000,
					   idle_timeout, ctx);
	}
	ctx->keepalive_to = timeout_add(KEEPALIVE_TIMEOUT * 1000,
					keepalive_timeout, ctx);

	str = getenv("MAILBOX_IDLE_CHECK_INTERVAL");
	interval = str == NULL ? 0 : (unsigned int)strtoul(str, NULL, 10);
	if (interval == 0)
		interval = DEFAULT_IDLE_CHECK_INTERVAL;

	if (client->mailbox != NULL) {
		mailbox_notify_changes(client->mailbox, interval,
				       idle_callback, ctx);
	}
	client_send_line(client, "+ idling");

	io_remove(&client->io);
	client->io = io_add(i_stream_get_fd(client->input),
			    IO_READ, idle_client_input, ctx);

	client->command_pending = TRUE;
	cmd->func = cmd_idle_continue;
	cmd->context = ctx;

	/* check immediately if there are changes. if they came before we
	   added mailbox-notifier, we wouldn't see them otherwise. */
	if (client->mailbox != NULL)
		idle_sync_now(client->mailbox, ctx);
	return FALSE;
}
