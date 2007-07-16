#ifndef __MAIL_INDEX_TRANSACTION_PRIVATE_H
#define __MAIL_INDEX_TRANSACTION_PRIVATE_H

#include "seq-range-array.h"
#include "mail-transaction-log.h"

struct mail_index_transaction_keyword_update {
	ARRAY_TYPE(seq_range) add_seq;
	ARRAY_TYPE(seq_range) remove_seq;
};

struct mail_index_transaction_ext_hdr_update {
	uint32_t ext_id;
	uint16_t offset;
	uint16_t size;
	/* unsigned char data[]; */
};

struct mail_index_transaction_vfuncs {
	int (*commit)(struct mail_index_transaction *t,
		      uint32_t *log_file_seq_r, uoff_t *log_file_offset_r);
	void (*rollback)(struct mail_index_transaction *t);
};

union mail_index_transaction_module_context {
	struct mail_index_module_register *reg;
};

struct mail_index_transaction {
	int refcount;

	struct mail_index_transaction_vfuncs v;
	struct mail_index_view *view;

	/* NOTE: If you add anything new, remember to update
	   mail_index_transaction_reset() to reset it. */
        ARRAY_DEFINE(appends, struct mail_index_record);
	uint32_t first_new_seq, last_new_seq;

	ARRAY_TYPE(seq_range) expunges;
	ARRAY_DEFINE(updates, struct mail_transaction_flag_update);
	size_t last_update_idx;

	unsigned char pre_hdr_change[sizeof(struct mail_index_header)];
	unsigned char pre_hdr_mask[sizeof(struct mail_index_header)];
	unsigned char post_hdr_change[sizeof(struct mail_index_header)];
	unsigned char post_hdr_mask[sizeof(struct mail_index_header)];

	ARRAY_DEFINE(ext_hdr_updates,
		     struct mail_index_transaction_ext_hdr_update *);
	ARRAY_DEFINE(ext_rec_updates, ARRAY_TYPE(seq_array));
	ARRAY_DEFINE(ext_resizes, struct mail_transaction_ext_intro);
	ARRAY_DEFINE(ext_resets, uint32_t);
	ARRAY_DEFINE(ext_reset_ids, uint32_t);

	ARRAY_DEFINE(keyword_updates,
		     struct mail_index_transaction_keyword_update);
	ARRAY_TYPE(seq_range) keyword_resets;

        struct mail_cache_transaction_ctx *cache_trans_ctx;

	/* Module-specific contexts. */
	ARRAY_DEFINE(module_contexts,
		     union mail_index_transaction_module_context *);

	/* this transaction was created for index_sync_view view */
	unsigned int sync_transaction:1;
	unsigned int hide_transaction:1;
	unsigned int no_appends:1;
	unsigned int external:1;

	unsigned int appends_nonsorted:1;
	unsigned int pre_hdr_changed:1;
	unsigned int post_hdr_changed:1;
	unsigned int reset:1;
	/* non-extension updates */
	unsigned int log_updates:1;
	/* extension updates */
	unsigned int log_ext_updates:1;
};

extern void (*hook_mail_index_transaction_created)
		(struct mail_index_transaction *t);

struct mail_index_record *
mail_index_transaction_lookup(struct mail_index_transaction *t, uint32_t seq);

void mail_index_transaction_ref(struct mail_index_transaction *t);
void mail_index_transaction_unref(struct mail_index_transaction **t);

void mail_index_transaction_sort_appends(struct mail_index_transaction *t);
uint32_t mail_index_transaction_get_next_uid(struct mail_index_transaction *t);

bool mail_index_seq_array_lookup(const ARRAY_TYPE(seq_array) *array,
				 uint32_t seq, unsigned int *idx_r);

#endif
