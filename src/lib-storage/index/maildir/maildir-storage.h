#ifndef __MAILDIR_STORAGE_H
#define __MAILDIR_STORAGE_H

/* Hierarchy separator in Maildir++ filenames - shouldn't be changed */
#define MAILDIR_FS_SEP '.'
#define MAILDIR_FS_SEP_S "."

#define SUBSCRIPTION_FILE_NAME "subscriptions"
#define MAILDIR_INDEX_PREFIX "dovecot.index"

#include "index-storage.h"

struct maildir_save_context;
struct maildir_copy_context;

struct maildir_transaction_context {
	struct index_transaction_context ictx;
	struct maildir_save_context *save_ctx;
	struct maildir_copy_context *copy_ctx;
};

extern struct mail maildir_mail;

/* Return -1 = error, 0 = file not found, 1 = ok */
typedef int maildir_file_do_func(struct index_mailbox *ibox,
				 const char *path, void *context);

int maildir_file_do(struct index_mailbox *ibox, uint32_t seq,
		    maildir_file_do_func *func, void *context);
const char *maildir_generate_tmp_filename(const struct timeval *tv);
int maildir_create_tmp(struct index_mailbox *ibox, const char *dir,
		       mode_t mode, const char **fname_r);

struct mailbox_list_context *
maildir_mailbox_list_init(struct mail_storage *storage,
			  const char *mask, enum mailbox_list_flags flags);
int maildir_mailbox_list_deinit(struct mailbox_list_context *ctx);
struct mailbox_list *
maildir_mailbox_list_next(struct mailbox_list_context *ctx);

int maildir_storage_sync(struct mailbox *box, enum mailbox_sync_flags flags);
int maildir_storage_sync_readonly(struct index_mailbox *ibox);

struct mailbox_transaction_context *
maildir_transaction_begin(struct mailbox *box, int hide);
int maildir_transaction_commit(struct mailbox_transaction_context *t);
void maildir_transaction_rollback(struct mailbox_transaction_context *t);

int maildir_save(struct mailbox_transaction_context *t,
		 const struct mail_full_flags *flags,
		 time_t received_date, int timezone_offset,
		 const char *from_envelope, struct istream *data);
int maildir_save_commit(struct maildir_save_context *ctx);
void maildir_save_rollback(struct maildir_save_context *ctx);

int maildir_copy(struct mailbox_transaction_context *t, struct mail *mail);
int maildir_copy_commit(struct maildir_copy_context *ctx);
void maildir_copy_rollback(struct maildir_copy_context *ctx);

const char *maildir_fix_mailbox_name(struct index_storage *storage,
				     const char *name, int remove_namespace);
const char *maildir_get_path(struct index_storage *storage, const char *name);

int maildir_sync_last_commit(struct index_mailbox *ibox);

int maildir_filename_get_flags(const char *fname, enum mail_flags *flags_r,
			       keywords_mask_t keywords_r);
const char *maildir_filename_set_flags(const char *fname, enum mail_flags flags,
				       keywords_mask_t keywords);

unsigned int maildir_hash(const void *p);
int maildir_cmp(const void *p1, const void *p2);

#endif
