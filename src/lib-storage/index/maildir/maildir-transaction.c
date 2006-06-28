/* Copyright (C) 2004 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "maildir-storage.h"

static void (*next_hook_mail_index_transaction_created)
	(struct mail_index_transaction *t) = NULL;

static int maildir_transaction_commit(struct mail_index_transaction *t,
				      uint32_t *log_file_seq_r,
				      uoff_t *log_file_offset_r)
{
	struct maildir_transaction_context *mt = MAIL_STORAGE_TRANSACTION(t);
	struct maildir_mailbox *mbox = (struct maildir_mailbox *)mt->ictx.ibox;
	struct maildir_save_context *save_ctx;
	bool external = t->external;
	int ret = 0;

	if (mt->save_ctx != NULL) {
		if (maildir_transaction_save_commit_pre(mt->save_ctx) < 0) {
			mt->save_ctx = NULL;
			ret = -1;
		}
	}

	save_ctx = mt->save_ctx;

	if (index_transaction_finish_commit(&mt->ictx, log_file_seq_r,
					    log_file_offset_r) < 0)
		ret = -1;

	/* transaction is destroyed now. */
	mt = NULL;

	if (save_ctx != NULL)
		maildir_transaction_save_commit_post(save_ctx);

	if (ret == 0 && !external)
		ret = maildir_sync_last_commit(mbox);
	return ret;
}

static void maildir_transaction_rollback(struct mail_index_transaction *t)
{
	struct maildir_transaction_context *mt = MAIL_STORAGE_TRANSACTION(t);

	if (mt->save_ctx != NULL)
		maildir_transaction_save_rollback(mt->save_ctx);
	index_transaction_finish_rollback(&mt->ictx);
}

void maildir_transaction_created(struct mail_index_transaction *t)
{
	struct mailbox *box = MAIL_STORAGE_INDEX(t->view->index);

	if (strcmp(box->storage->name, MAILDIR_STORAGE_NAME) == 0) {
		struct maildir_mailbox *mbox = (struct maildir_mailbox *)box;
		struct maildir_transaction_context *mt;

		mt = i_new(struct maildir_transaction_context, 1);
		mt->ictx.trans = t;
		mt->ictx.super = t->v;

		t->v.commit = maildir_transaction_commit;
		t->v.rollback = maildir_transaction_rollback;

		array_idx_set(&t->mail_index_transaction_module_contexts,
			      mail_storage_mail_index_module_id, &mt);

		index_transaction_init(&mt->ictx, &mbox->ibox);
	}
	if (next_hook_mail_index_transaction_created != NULL)
		next_hook_mail_index_transaction_created(t);
}

void maildir_transaction_class_init(void)
{
	next_hook_mail_index_transaction_created =
		hook_mail_index_transaction_created;
	hook_mail_index_transaction_created = maildir_transaction_created;
}

void maildir_transaction_class_deinit(void)
{
	i_assert(hook_mail_index_transaction_created ==
		 maildir_transaction_created);
	hook_mail_index_transaction_created =
		next_hook_mail_index_transaction_created;
}
