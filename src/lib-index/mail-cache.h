#ifndef __MAIL_CACHE_H
#define __MAIL_CACHE_H

#include "mail-index.h"

#define MAIL_CACHE_FILE_PREFIX ".cache"

struct mail_cache;
struct mail_cache_view;
struct mail_cache_transaction_ctx;

enum mail_cache_decision_type {
	/* Not needed currently */
	MAIL_CACHE_DECISION_NO		= 0x00,
	/* Needed only for new mails. Drop when compressing. */
	MAIL_CACHE_DECISION_TEMP	= 0x01,
	/* Needed. */
	MAIL_CACHE_DECISION_YES		= 0x02,

	/* This decision has been forced manually, don't change it. */
	MAIL_CACHE_DECISION_FORCED	= 0x80
};

enum mail_cache_field_type {
	MAIL_CACHE_FIELD_FIXED_SIZE,
	MAIL_CACHE_FIELD_VARIABLE_SIZE,
	MAIL_CACHE_FIELD_STRING,
	MAIL_CACHE_FIELD_BITMASK,
	MAIL_CACHE_FIELD_HEADER
};

struct mail_cache_field {
	const char *name;
	unsigned int idx;

	enum mail_cache_field_type type;
	unsigned int field_size;
	enum mail_cache_decision_type decision;

	/* internal: */
	uint32_t uid_highwater;
	time_t last_used;
};

struct mail_cache *mail_cache_open_or_create(struct mail_index *index);
void mail_cache_free(struct mail_cache *cache);

/* Register fields. fields[].idx is updated to contain field index. */
void mail_cache_register_fields(struct mail_cache *cache,
				struct mail_cache_field *fields,
				size_t fields_count);
/* Returns registered field index, or (unsigned int)-1 if not found. */
unsigned int
mail_cache_register_lookup(struct mail_cache *cache, const char *name);

/* Returns TRUE if cache should be compressed. */
int mail_cache_need_compress(struct mail_cache *cache);
/* Compress cache file. */
int mail_cache_compress(struct mail_cache *cache, struct mail_index_view *view);

struct mail_cache_view *
mail_cache_view_open(struct mail_cache *cache, struct mail_index_view *iview);
void mail_cache_view_close(struct mail_cache_view *view);

/* Get index transaction specific cache transaction. */
struct mail_cache_transaction_ctx *
mail_cache_get_transaction(struct mail_cache_view *view,
			   struct mail_index_transaction *t);

/* Add new field to given record. Updates are not allowed. Fixed size fields
   must be exactly the expected size. */
void mail_cache_add(struct mail_cache_transaction_ctx *ctx, uint32_t seq,
		    unsigned int field, const void *data, size_t data_size);

/* Retursn 1 if field exists, 0 if not, -1 if error. */
int mail_cache_field_exists(struct mail_cache_view *view, uint32_t seq,
			    unsigned int field);
/* Returns current caching decision for given field. */
enum mail_cache_decision_type
mail_cache_field_get_decision(struct mail_cache *cache, unsigned int field);

/* Set data_r and size_r to point to wanted field in cache file.
   Returns 1 if field was found, 0 if not, -1 if error. */
int mail_cache_lookup_field(struct mail_cache_view *view, buffer_t *dest_buf,
			    uint32_t seq, unsigned int field);

/* Return specified cached headers. Returns 1 if all fields were found,
   0 if not, -1 if error. dest is updated only if all fields were found. */
int mail_cache_lookup_headers(struct mail_cache_view *view, string_t *dest,
			      uint32_t seq, unsigned int fields[],
			      size_t fields_count);

/* "Error in index cache file %s: ...". */
void mail_cache_set_corrupted(struct mail_cache *cache, const char *fmt, ...)
	__attr_format__(2, 3);

#endif
