/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "commands.h"

int cmd_create(struct client_command_context *cmd)
{
	struct mail_storage *storage;
	const char *mailbox, *full_mailbox;
	int directory;
	size_t len;

	/* <mailbox> */
	if (!client_read_string_args(cmd, 1, &mailbox))
		return FALSE;
	full_mailbox = mailbox;

	storage = client_find_storage(cmd, &mailbox);
	if (storage == NULL)
		return TRUE;

	len = strlen(mailbox);
	if (len == 0 ||
	    mailbox[len-1] != mail_storage_get_hierarchy_sep(storage))
		directory = FALSE;
	else {
		/* name ends with hierarchy separator - client is just
		   informing us that it wants to create children under this
		   mailbox. */
                directory = TRUE;
		mailbox = t_strndup(mailbox, len-1);
		full_mailbox = t_strndup(full_mailbox, strlen(full_mailbox)-1);
	}

	if (!client_verify_mailbox_name(cmd, full_mailbox, FALSE, TRUE))
		return TRUE;

	if (mail_storage_mailbox_create(storage, mailbox, directory) < 0)
		client_send_storage_error(cmd, storage);
	else
		client_send_tagline(cmd, "OK Create completed.");
	return TRUE;
}
