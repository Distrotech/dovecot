/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "commands.h"

static int fetch_and_copy(struct mail_copy_context *copy_ctx,
			  struct mailbox *box, const char *messageset,
			  int uidset, int *all_found)
{
	struct mail_fetch_context *fetch_ctx;
	struct mail *mail;
	int failed = FALSE;

	fetch_ctx = box->fetch_init(box, MAIL_FETCH_STREAM_HEADER |
				    MAIL_FETCH_STREAM_BODY, NULL,
				    messageset, uidset);
	if (fetch_ctx == NULL)
		return FALSE;

	while ((mail = box->fetch_next(fetch_ctx)) != NULL) {
		if (!mail->copy(mail, copy_ctx)) {
			failed = TRUE;
			break;
		}
	}

	if (!box->fetch_deinit(fetch_ctx, all_found))
		return FALSE;

	return !failed;
}

int cmd_copy(struct client *client)
{
	struct mail_storage *storage;
	struct mailbox *destbox;
        struct mail_copy_context *copy_ctx;
	const char *messageset, *mailbox;
	int failed = FALSE, all_found = TRUE;

	/* <message set> <mailbox> */
	if (!client_read_string_args(client, 2, &messageset, &mailbox))
		return FALSE;

	if (!client_verify_mailbox_name(client, mailbox, TRUE, FALSE))
		return TRUE;

	/* open the destination mailbox */
	if (!client_verify_mailbox_name(client, mailbox, TRUE, FALSE))
		return TRUE;

	storage = client_find_storage(client, mailbox);
	if (storage == NULL)
		return TRUE;

	destbox = storage->open_mailbox(storage, mailbox,
					mailbox_open_flags | MAILBOX_OPEN_FAST);
	if (destbox == NULL) {
		client_send_storage_error(client, storage);
		return TRUE;
	}

	if (destbox == client->mailbox) {
		/* copying inside same mailbox, make sure we get the
		   locking right */
		if (!destbox->lock(destbox, MAILBOX_LOCK_READ |
				   MAILBOX_LOCK_SAVE))
			failed = TRUE;
	}

	copy_ctx = failed ? NULL : destbox->copy_init(destbox);
	if (copy_ctx == NULL)
		failed = TRUE;
	else {
		if (!fetch_and_copy(copy_ctx, client->mailbox,
				    messageset, client->cmd_uid, &all_found))
			failed = TRUE;

		if (!destbox->copy_deinit(copy_ctx, failed || !all_found))
			failed = TRUE;
	}

	(void)destbox->lock(destbox, MAILBOX_LOCK_UNLOCK);

	if (failed)
		client_send_storage_error(client, storage);
	else if (!all_found) {
		/* some messages were expunged, sync them */
		client_sync_full(client);
		client_send_tagline(client,
			"NO Some of the requested messages no longer exist.");
	} else {
		if (destbox == client->mailbox)
			client_sync_full(client);
		else
			client_sync_full_fast(client);
		client_send_tagline(client, "OK Copy completed.");
	}

	destbox->close(destbox);
	return TRUE;
}
