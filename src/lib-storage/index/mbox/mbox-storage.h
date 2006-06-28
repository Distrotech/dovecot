#ifndef __MBOX_STORAGE_H
#define __MBOX_STORAGE_H

/* Padding to leave in X-Keywords header when rewriting mbox */
#define MBOX_HEADER_PADDING 50
/* Don't write Content-Length header unless it's value is larger than this. */
#define MBOX_MIN_CONTENT_LENGTH_SIZE 1024

#define MBOX_STORAGE_NAME "mbox"
#define SUBSCRIPTION_FILE_NAME ".subscriptions"
#define MBOX_INDEX_PREFIX "dovecot.index"
#define MBOX_INDEX_DIR_NAME ".imap"

#include "index-storage.h"

#define STORAGE(mbox_storage) \
	(&(mbox_storage)->storage.storage)
#define INDEX_STORAGE(mbox_storage) \
	(&(mbox_storage)->storage)

struct mbox_storage {
	struct index_storage storage;
};

struct mbox_mailbox {
	struct index_mailbox ibox;
	struct mbox_storage *storage;

	const char *path;

	int mbox_fd;
	struct istream *mbox_stream, *mbox_file_stream;
	int mbox_lock_type;
	dev_t mbox_dev;
	ino_t mbox_ino;
	unsigned int mbox_excl_locks, mbox_shared_locks;
	struct dotlock *mbox_dotlock;
	unsigned int mbox_lock_id, mbox_global_lock_id;
	bool mbox_readonly, mbox_writeonly;
	time_t mbox_dirty_stamp;
	off_t mbox_dirty_size;

	uint32_t mbox_ext_idx;

	unsigned int no_mbox_file:1;
	unsigned int mbox_sync_dirty:1;
	unsigned int mbox_do_dirty_syncs:1;
	unsigned int mbox_very_dirty_syncs:1;
	unsigned int mbox_save_md5:1;
	unsigned int mbox_dotlocked:1;
};

struct mbox_transaction_context {
	struct index_transaction_context ictx;

	struct mbox_save_context *save_ctx;
	unsigned int mbox_lock_id;
	unsigned int mbox_modified:1;
};

extern struct mail_vfuncs mbox_mail_vfuncs;
extern const char *mbox_hide_headers[];
extern unsigned int mbox_hide_headers_count;

int mbox_set_syscall_error(struct mbox_mailbox *mbox, const char *function);

struct mailbox_list_context *
mbox_mailbox_list_init(struct mail_storage *storage,
		       const char *ref, const char *mask,
		       enum mailbox_list_flags flags);
int mbox_mailbox_list_deinit(struct mailbox_list_context *ctx);
struct mailbox_list *mbox_mailbox_list_next(struct mailbox_list_context *ctx);

void mbox_transaction_created(struct mail_index_transaction *t);
void mbox_transaction_class_init(void);
void mbox_transaction_class_deinit(void);

struct mailbox_sync_context *
mbox_storage_sync_init(struct mailbox *box, enum mailbox_sync_flags flags);

int mbox_save_init(struct mailbox_transaction_context *_t,
		   enum mail_flags flags, struct mail_keywords *keywords,
		   time_t received_date, int timezone_offset,
		   const char *from_envelope, struct istream *input,
		   struct mail *dest_mail, struct mail_save_context **ctx_r);
int mbox_save_continue(struct mail_save_context *ctx);
int mbox_save_finish(struct mail_save_context *ctx);
void mbox_save_cancel(struct mail_save_context *ctx);

int mbox_transaction_save_commit(struct mbox_save_context *ctx);
void mbox_transaction_save_rollback(struct mbox_save_context *ctx);

bool mbox_is_valid_mask(struct mail_storage *storage, const char *mask);

#endif
