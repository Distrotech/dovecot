/* Copyright (c) 2002-2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "index-storage.h"
#include "mail-index-modseq.h"

void index_storage_get_status(struct mailbox *box,
			      enum mailbox_status_items items,
			      struct mailbox_status *status_r)
{
	struct index_mailbox *ibox = (struct index_mailbox *)box;
	const struct mail_index_header *hdr;

	if (!box->opened)
		index_storage_mailbox_open(ibox);

	memset(status_r, 0, sizeof(struct mailbox_status));

	/* we can get most of the status items without any trouble */
	hdr = mail_index_get_header(ibox->view);
	status_r->messages = hdr->messages_count;
	if ((items & STATUS_RECENT) != 0) {
		status_r->recent = index_mailbox_get_recent_count(ibox);
		i_assert(status_r->recent <= status_r->messages);
	}
	status_r->unseen = hdr->messages_count - hdr->seen_messages_count;
	status_r->uidvalidity = hdr->uid_validity;
	status_r->uidnext = hdr->next_uid;
	status_r->nonpermanent_modseqs = mail_index_is_in_memory(ibox->index);
	if ((items & STATUS_HIGHESTMODSEQ) != 0) {
		status_r->highest_modseq =
			mail_index_modseq_get_highest(ibox->view);
		if (status_r->highest_modseq == 0) {
			/* modseqs not enabled yet, but we can't return 0 */
			status_r->highest_modseq = 1;
		}
	}

	if (items & STATUS_FIRST_UNSEEN_SEQ) {
		mail_index_lookup_first(ibox->view, 0, MAIL_SEEN,
					&status_r->first_unseen_seq);
	}

	if (items & STATUS_KEYWORDS)
		status_r->keywords = mail_index_get_keywords(ibox->index);
}
