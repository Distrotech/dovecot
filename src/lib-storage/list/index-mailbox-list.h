#ifndef __INDEX_MAILBOX_LIST_H
#define __INDEX_MAILBOX_LIST_H

#include "mailbox-list-private.h"

#define MAIL_INDEX_PREFIX "dovecot.list.index"
#define MAILBOX_LIST_INDEX_NAME MAIL_INDEX_PREFIX".uidmap"

#define INDEX_LIST_CONTEXT(obj) \
	*((void **)array_idx_modifiable(&(obj)->module_contexts, \
					index_mailbox_list_module_id))

struct index_mailbox_list {
	struct mailbox_list_vfuncs super;

	struct mail_index *mail_index;
	struct mailbox_list_index *list_index;

	uint32_t eid_messages, eid_unseen, eid_recent;
	uint32_t eid_uid_validity, eid_uidnext;

	uint32_t eid_cur_sync_stamp, eid_new_sync_stamp, eid_dirty_flags;
};

struct index_mailbox_list_iterate_context {
	struct mailbox_list_iterate_context ctx;

	struct mailbox_list_iter_ctx *iter_ctx;
	struct mailbox_list_index_sync_ctx *sync_ctx;
	struct mailbox_list_iterate_context *backend_ctx;

	struct mail_index_view *view;
	struct mail_index_transaction *trans;

	char *prefix;
	int recurse_level;
	struct imap_match_glob *glob;

	pool_t info_pool;
	struct mailbox_info info;
	uint32_t sync_stamp;

	unsigned int failed:1;
};

extern unsigned int index_mailbox_list_module_id;

void index_mailbox_list_sync_init(void);
void index_mailbox_list_sync_init_list(struct mailbox_list *list);

#endif
