#ifndef __MAIL_INDEX_SYNC_PRIVATE_H
#define __MAIL_INDEX_SYNC_PRIVATE_H

#include "mail-transaction-log.h"

struct mail_index_sync_ctx {
	struct mail_index *index;
	struct mail_index_view *view;
	struct mail_index_transaction *trans;

	const struct mail_transaction_header *hdr;
	const void *data;

	array_t ARRAY_DEFINE(sync_list, struct mail_index_sync_list);
	uint32_t next_uid;

	uint32_t append_uid_first, append_uid_last;

	unsigned int lock_id;

	unsigned int sync_appends:1;
	unsigned int sync_recent:1;
	unsigned int sync_dirty:1;
};

struct mail_index_sync_list {
	const array_t *ARRAY_DEFINE_PTR(array, struct uid_range);
	unsigned int idx;
	unsigned int keyword_idx:31;
	unsigned int keyword_remove:1;
};

struct mail_index_expunge_handler {
	mail_index_expunge_handler_t *handler;
	void **context;
	uint32_t record_offset;
};

struct mail_index_sync_map_ctx {
	struct mail_index_view *view;
	uint32_t cur_ext_id;

	array_t ARRAY_DEFINE(expunge_handlers,
			     struct mail_index_expunge_handler);

	array_t ARRAY_DEFINE(extra_contexts, void *);

        enum mail_index_sync_handler_type type;

	unsigned int sync_handlers_initialized:1;
	unsigned int expunge_handlers_set:1;
	unsigned int expunge_handlers_used:1;
	unsigned int cur_ext_ignore:1;
	unsigned int keywords_read:1;
	unsigned int unreliable_flags:1;
};

extern struct mail_transaction_map_functions mail_index_map_sync_funcs;

void mail_index_sync_map_init(struct mail_index_sync_map_ctx *sync_map_ctx,
			      struct mail_index_view *view,
			      enum mail_index_sync_handler_type type);
void mail_index_sync_map_deinit(struct mail_index_sync_map_ctx *sync_map_ctx);
int mail_index_sync_update_index(struct mail_index_sync_ctx *sync_ctx,
				 bool sync_only_external);

int mail_index_sync_record(struct mail_index_sync_map_ctx *ctx,
			   const struct mail_transaction_header *hdr,
			   const void *data);

void mail_index_sync_replace_map(struct mail_index_sync_map_ctx *ctx,
				 struct mail_index_map *map);

void mail_index_sync_init_expunge_handlers(struct mail_index_sync_map_ctx *ctx);
void
mail_index_sync_deinit_expunge_handlers(struct mail_index_sync_map_ctx *ctx);
void mail_index_sync_init_handlers(struct mail_index_sync_map_ctx *ctx);
void mail_index_sync_deinit_handlers(struct mail_index_sync_map_ctx *ctx);

int mail_index_sync_ext_intro(struct mail_index_sync_map_ctx *ctx,
			      const struct mail_transaction_ext_intro *u);
int mail_index_sync_ext_reset(struct mail_index_sync_map_ctx *ctx,
			      const struct mail_transaction_ext_reset *u);
int
mail_index_sync_ext_hdr_update(struct mail_index_sync_map_ctx *ctx,
			       const struct mail_transaction_ext_hdr_update *u);
int
mail_index_sync_ext_rec_update(struct mail_index_sync_map_ctx *ctx,
			       const struct mail_transaction_ext_rec_update *u);

int mail_index_sync_keywords(struct mail_index_sync_map_ctx *ctx,
			     const struct mail_transaction_header *hdr,
			     const struct mail_transaction_keyword_update *rec);
int
mail_index_sync_keywords_reset(struct mail_index_sync_map_ctx *ctx,
			       const struct mail_transaction_header *hdr,
			       const struct mail_transaction_keyword_reset *r);

#endif
