#ifndef LUCENE_WRAPPER_H
#define LUCENE_WRAPPER_H

#include "fts-api-private.h"

#define MAILBOX_GUID_HEX_LENGTH (MAIL_GUID_128_SIZE*2)

struct lucene_index *lucene_index_init(const char *path);
void lucene_index_deinit(struct lucene_index *index);

void lucene_index_select_mailbox(struct lucene_index *index,
				 const wchar_t guid[MAILBOX_GUID_HEX_LENGTH]);
void lucene_index_unselect_mailbox(struct lucene_index *index);
int lucene_index_get_last_uid(struct lucene_index *index, uint32_t *last_uid_r);
int lucene_index_get_doc_count(struct lucene_index *index, uint32_t *count_r);

int lucene_index_build_init(struct lucene_index *index);
int lucene_index_build_more(struct lucene_index *index, uint32_t uid,
			    const unsigned char *data, size_t size,
			    const char *hdr_name);
int lucene_index_build_deinit(struct lucene_index *index);

int lucene_index_optimize_scan(struct lucene_index *index,
			       const ARRAY_TYPE(seq_range) *existing_uids,
			       ARRAY_TYPE(seq_range) *missing_uids_r);
int lucene_index_optimize_finish(struct lucene_index *index);

int lucene_index_lookup(struct lucene_index *index, 
			struct mail_search_arg *args, bool and_args,
			struct fts_result *result);

int lucene_index_lookup_multi(struct lucene_index *index,
			      struct hash_table *guids,
			      struct mail_search_arg *args, bool and_args,
			      struct fts_multi_result *result);

#endif
