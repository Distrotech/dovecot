#ifndef __MAIL_INDEX_SYNC_PRIVATE_H
#define __MAIL_INDEX_SYNC_PRIVATE_H

struct mail_index_sync_ctx {
	struct mail_index *index;
	struct mail_index_view *view;

	buffer_t *expunges_buf, *updates_buf, *appends_buf;

	const struct mail_transaction_expunge *expunges;
	const struct mail_transaction_flag_update *updates;
	size_t expunges_count, updates_count;

	const struct mail_transaction_header *hdr;
	const void *data;

	size_t expunge_idx, update_idx;
	uint32_t next_uid;

	unsigned int lock_id;

	unsigned int sync_appends:1;
};

extern struct mail_transaction_map_functions mail_index_map_sync_funcs;

int mail_index_sync_update_index(struct mail_index_sync_ctx *sync_ctx);

void
mail_index_sync_get_expunge(struct mail_index_sync_rec *rec,
			    const struct mail_transaction_expunge *exp);
void
mail_index_sync_get_update(struct mail_index_sync_rec *rec,
			   const struct mail_transaction_flag_update *update);

#endif
