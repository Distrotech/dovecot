#ifndef __MAILDIR_STORAGE_H
#define __MAILDIR_STORAGE_H

#define MAILDIR_STORAGE_NAME "maildir"
#define MAILDIR_SUBSCRIPTION_FILE_NAME "subscriptions"
#define MAILDIR_INDEX_PREFIX "dovecot.index"
#define MAILDIR_UNLINK_DIRNAME "DOVECOT-TRASHED"

/* "base,S=123:2," means:
   <base> [<extra sep> <extra data> [..]] <info sep> 2 <flags sep> */
#define MAILDIR_INFO_SEP ':'
#define MAILDIR_EXTRA_SEP ','
#define MAILDIR_FLAGS_SEP ','

#define MAILDIR_INFO_SEP_S ":"
#define MAILDIR_EXTRA_SEP_S ","
#define MAILDIR_FLAGS_SEP_S ","

/* ":2," is the standard flags separator */
#define MAILDIR_FLAGS_FULL_SEP MAILDIR_INFO_SEP_S "2" MAILDIR_FLAGS_SEP_S

#define MAILDIR_KEYWORD_FIRST 'a'
#define MAILDIR_KEYWORD_LAST 'z'
#define MAILDIR_MAX_KEYWORDS (MAILDIR_KEYWORD_LAST - MAILDIR_KEYWORD_FIRST + 1)

/* Maildir++ extension: include file size in the filename to avoid stat() */
#define MAILDIR_EXTRA_FILE_SIZE 'S'
/* Something (can't remember what anymore) could use 'W' in filename to avoid
   calculating file's virtual size (added missing CRs). */
#define MAILDIR_EXTRA_VIRTUAL_SIZE 'W'

#define MAILDIR_SAVE_FLAG_HARDLINK 0x10000000
#define MAILDIR_SAVE_FLAG_DELETED  0x20000000

#include "index-storage.h"

#define STORAGE(maildir_storage) \
	(&(maildir_storage)->storage.storage)
#define INDEX_STORAGE(maildir_storage) \
	(&(maildir_storage)->storage)

struct timeval;
struct maildir_save_context;
struct maildir_copy_context;

struct maildir_storage {
	struct index_storage storage;

	const char *temp_prefix;

	unsigned int copy_with_hardlinks:1;
	unsigned int save_size_in_filename:1;
	unsigned int stat_dirs:1;
};

enum maildir_dirty_flags {
	MAILDIR_DIRTY_NEW = 0x01,
	MAILDIR_DIRTY_CUR = 0x02
};

struct maildir_mailbox {
	struct index_mailbox ibox;
	struct maildir_storage *storage;

	const char *path, *control_dir;

	/* maildir sync: */
	struct maildir_uidlist *uidlist;
	struct maildir_keywords *keywords;
	time_t last_new_mtime, last_cur_mtime;
	time_t dirty_cur_time;
	enum maildir_dirty_flags last_dirty_flags;

        mode_t mail_create_mode;
	unsigned int private_flags_mask;

	unsigned int syncing_commit:1;
};

struct maildir_transaction_context {
	struct index_transaction_context ictx;
	struct maildir_save_context *save_ctx;
};

extern struct mail_vfuncs maildir_mail_vfuncs;

/* Return -1 = error, 0 = file not found, 1 = ok */
typedef int maildir_file_do_func(struct maildir_mailbox *mbox,
				 const char *path, void *context);

int maildir_file_do(struct maildir_mailbox *mbox, uint32_t uid,
		    maildir_file_do_func *callback, void *context);
#ifdef CONTEXT_TYPE_SAFETY
#  define maildir_file_do(mbox, seq, callback, context) \
	({(void)(1 ? 0 : callback((struct maildir_mailbox *)NULL, \
				  (const char *)NULL, context)); \
	  maildir_file_do(mbox, seq, \
		(maildir_file_do_func *)callback, context); })
#else
#  define maildir_file_do(mbox, seq, callback, context) \
	maildir_file_do(mbox, seq, (maildir_file_do_func *)callback, context)
#endif

const char *maildir_generate_tmp_filename(const struct timeval *tv);
int maildir_create_tmp(struct maildir_mailbox *mbox, const char *dir,
		       mode_t mode, const char **fname_r);
bool maildir_filename_get_size(const char *fname, char type, uoff_t *size_r);

void maildir_transaction_created(struct mail_index_transaction *t);
void maildir_transaction_class_init(void);
void maildir_transaction_class_deinit(void);

int maildir_save_init(struct mailbox_transaction_context *_t,
		      enum mail_flags flags, struct mail_keywords *keywords,
		      time_t received_date, int timezone_offset,
		      const char *from_envelope, struct istream *input,
		      struct mail *dest_mail, struct mail_save_context **ctx_r);
int maildir_save_continue(struct mail_save_context *ctx);
int maildir_save_finish(struct mail_save_context *ctx);
void maildir_save_cancel(struct mail_save_context *ctx);

struct maildir_save_context *
maildir_save_transaction_init(struct maildir_transaction_context *t);
uint32_t maildir_save_add(struct maildir_transaction_context *t,
			  const char *base_fname, enum mail_flags flags,
			  struct mail_keywords *keywords,
			  struct mail *dest_mail);
const char *maildir_save_file_get_path(struct mailbox_transaction_context *t,
				       uint32_t seq);

int maildir_transaction_save_commit_pre(struct maildir_save_context *ctx);
void maildir_transaction_save_commit_post(struct maildir_save_context *ctx);
void maildir_transaction_save_rollback(struct maildir_save_context *ctx);

int maildir_copy(struct mailbox_transaction_context *t, struct mail *mail,
		 enum mail_flags flags, struct mail_keywords *keywords,
		 struct mail *dest_mail);
int maildir_transaction_copy_commit(struct maildir_copy_context *ctx);
void maildir_transaction_copy_rollback(struct maildir_copy_context *ctx);

unsigned int maildir_hash(const void *p);
int maildir_cmp(const void *p1, const void *p2);

#endif
