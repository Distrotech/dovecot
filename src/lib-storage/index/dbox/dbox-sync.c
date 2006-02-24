/* Copyright (C) 2005 Timo Sirainen */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "hash.h"
#include "seq-range-array.h"
#include "write-full.h"
#include "dbox-file.h"
#include "dbox-keywords.h"
#include "dbox-sync.h"
#include "dbox-uidlist.h"
#include "dbox-storage.h"

#include <stddef.h>

int dbox_sync_get_file_offset(struct dbox_sync_context *ctx, uint32_t seq,
			      uint32_t *file_seq_r, uoff_t *offset_r)
{
	int ret;

	ret = dbox_file_lookup_offset(ctx->mbox, ctx->sync_view, seq,
				      file_seq_r, offset_r);
	if (ret <= 0) {
		if (ret == 0) {
			mail_storage_set_critical(STORAGE(ctx->mbox->storage),
				"Unexpectedly lost seq %u in "
				"dbox %s", seq, ctx->mbox->path);
		}
		return -1;
	}
	return 0;
}

static int dbox_sync_add_seq(struct dbox_sync_context *ctx, uint32_t seq,
                             const struct dbox_sync_rec *sync_rec)
{
        struct dbox_sync_file_entry *entry;
	uint32_t file_seq;
	uoff_t offset;

	if (dbox_sync_get_file_offset(ctx, seq, &file_seq, &offset) < 0)
		return -1;

	if (ctx->prev_file_seq == file_seq)
		return 0; /* already added in last sequence */
	ctx->prev_file_seq = file_seq;

	entry = hash_lookup(ctx->syncs, POINTER_CAST(file_seq));
	if (entry != NULL) {
		/* check if it's already added */
		const struct dbox_sync_rec *sync_recs;
		unsigned int count;

		sync_recs = array_get(&entry->sync_recs, &count);
		i_assert(count > 0);
		if (memcmp(&sync_recs[count-1],
			   sync_rec, sizeof(*sync_rec)) == 0)
			return 0; /* already added */
	} else {
		entry = p_new(ctx->pool, struct dbox_sync_file_entry, 1);
		entry->file_seq = file_seq;
		ARRAY_CREATE(&entry->sync_recs, ctx->pool,
			     struct dbox_sync_rec, 3);
		hash_insert(ctx->syncs, POINTER_CAST(file_seq), entry);
	}

	array_append(&entry->sync_recs, sync_rec, 1);
	return 0;
}

static int dbox_sync_add(struct dbox_sync_context *ctx,
			 const struct mail_index_sync_rec *sync_rec)
{
        struct dbox_sync_rec dbox_sync_rec;
	uint32_t seq, seq1, seq2;

	if (sync_rec->type == MAIL_INDEX_SYNC_TYPE_APPEND) {
		/* don't care about appends */
		return 0;
	}

	if (mail_index_lookup_uid_range(ctx->sync_view,
					sync_rec->uid1, sync_rec->uid2,
					&seq1, &seq2) < 0) {
		mail_storage_set_index_error(&ctx->mbox->ibox);
		return -1;
	}

	if (seq1 == 0) {
		/* already expunged everything. nothing to do. */
		return 0;
	}

	/* convert to dbox_sync_rec, which takes a bit less space and has
	   sequences instead of UIDs. */
	memset(&dbox_sync_rec, 0, sizeof(dbox_sync_rec));
	dbox_sync_rec.type = sync_rec->type;
	dbox_sync_rec.seq1 = seq1;
	dbox_sync_rec.seq2 = seq2;
	switch (sync_rec->type) {
	case MAIL_INDEX_SYNC_TYPE_FLAGS:
		dbox_sync_rec.value.flags.add = sync_rec->add_flags;
		dbox_sync_rec.value.flags.remove = sync_rec->remove_flags;
		break;
	case MAIL_INDEX_SYNC_TYPE_KEYWORD_ADD:
	case MAIL_INDEX_SYNC_TYPE_KEYWORD_REMOVE:
	case MAIL_INDEX_SYNC_TYPE_KEYWORD_RESET:
		dbox_sync_rec.value.keyword_idx = sync_rec->keyword_idx;
		break;
	case MAIL_INDEX_SYNC_TYPE_EXPUNGE:
	case MAIL_INDEX_SYNC_TYPE_APPEND:
		break;
	}

	/* now, add the same sync_rec to each file_seq's entry */
	ctx->prev_file_seq = 0;
	for (seq = seq1; seq <= seq2; seq++) {
		if (dbox_sync_add_seq(ctx, seq, &dbox_sync_rec) < 0)
			return -1;
	}
	return 0;
}

static int
dbox_sync_write_mask(struct dbox_sync_context *ctx,
		     const struct dbox_sync_rec *sync_rec,
                     unsigned int first_flag_offset, unsigned int flag_count,
		     const unsigned char *array, const unsigned char *mask)
{
	struct dbox_mailbox *mbox = ctx->mbox;
	uint32_t file_seq, uid2;
	uoff_t offset;
	unsigned int i, start;
	int ret;

	if (dbox_sync_get_file_offset(ctx, sync_rec->seq1,
				      &file_seq, &offset) < 0)
		return -1;

	if (mail_index_lookup_uid(ctx->sync_view, sync_rec->seq2, &uid2) < 0) {
		mail_storage_set_index_error(&ctx->mbox->ibox);
		return -1;
	}

	if ((ret = dbox_file_seek(mbox, file_seq, offset)) <= 0)
		return ret;

	while (mbox->file->seeked_uid <= uid2) {
		for (i = 0; i < flag_count; ) {
			if (!mask[i]) {
				i++;
				continue;
			}

			start = i;
			while (i < flag_count) {
				if (!mask[i])
					break;
				i++;
			}
			ret = pwrite_full(ctx->mbox->file->fd,
					  array + start, i - start,
					  offset + first_flag_offset + start);
			if (ret < 0) {
				mail_storage_set_critical(
					STORAGE(mbox->storage),
					"pwrite(%s) failed: %m",
					mbox->file->path);
				return -1;
			}
		}

		ret = dbox_file_seek_next_nonexpunged(mbox);
		if (ret <= 0) {
			if (ret == 0)
				break;
			return -1;
		}
		offset = mbox->file->seeked_offset;
	}
	return 0;
}

int dbox_sync_update_flags(struct dbox_sync_context *ctx,
			   const struct dbox_sync_rec *sync_rec)
{
	static enum mail_flags dbox_flag_list[] = {
		MAIL_ANSWERED,
		MAIL_FLAGGED,
		MAIL_DELETED,
		MAIL_SEEN,
		MAIL_DRAFT,
		0 /* expunged */
	};
#define DBOX_FLAG_COUNT (sizeof(dbox_flag_list)/sizeof(dbox_flag_list[0]))
	unsigned char dbox_flag_array[DBOX_FLAG_COUNT];
	unsigned char dbox_flag_mask[DBOX_FLAG_COUNT];
	unsigned int i, first_flag_offset;

	/* first build flag array and mask */
	if (sync_rec->type == MAIL_INDEX_SYNC_TYPE_EXPUNGE) {
		memset(dbox_flag_array, '0', sizeof(dbox_flag_array));
		memset(dbox_flag_mask, 0, sizeof(dbox_flag_mask));
		dbox_flag_mask[5] = 1;
		dbox_flag_array[5] = '1';
	} else {
		i_assert(sync_rec->type == MAIL_INDEX_SYNC_TYPE_FLAGS);
		for (i = 0; i < DBOX_FLAG_COUNT; i++) {
			dbox_flag_array[i] =
				(sync_rec->value.flags.add &
				 dbox_flag_list[i]) != 0 ? '1' : '0';
			dbox_flag_mask[i] = dbox_flag_array[i] ||
				(sync_rec->value.flags.remove &
				 dbox_flag_list[i]) != 0;
		}
	}
	first_flag_offset = offsetof(struct dbox_mail_header, answered);

	return dbox_sync_write_mask(ctx, sync_rec,
				    first_flag_offset, DBOX_FLAG_COUNT,
				    dbox_flag_array, dbox_flag_mask);
}

static int
dbox_sync_update_keyword(struct dbox_sync_context *ctx,
			 const struct dbox_sync_rec *sync_rec, bool set)
{
	unsigned char keyword_array, keyword_mask = 1;
	unsigned int file_idx, first_flag_offset;

	keyword_array = set ? '1' : '0';

	if (!dbox_file_lookup_keyword(ctx->mbox, ctx->mbox->file,
				      sync_rec->value.keyword_idx, &file_idx)) {
		/* not found. if removing, just ignore.

		   if adding, it currently happens only if the maximum keyword
		   count was reached. once we support moving mails to new file
		   to grow keywords count, this should never happen.
		   for now, just ignore this. */
		return 0;
	}

	first_flag_offset = sizeof(struct dbox_mail_header) + file_idx;
	return dbox_sync_write_mask(ctx, sync_rec, first_flag_offset, 1,
				    &keyword_array, &keyword_mask);
}

static int
dbox_sync_reset_keyword(struct dbox_sync_context *ctx,
			 const struct dbox_sync_rec *sync_rec)
{
	unsigned char *keyword_array, *keyword_mask;
	unsigned int first_flag_offset;
	int ret;

	if (ctx->mbox->file->keyword_count == 0)
		return 0;

	t_push();
	keyword_array = t_malloc(ctx->mbox->file->keyword_count);
	keyword_mask = t_malloc(ctx->mbox->file->keyword_count);
	memset(keyword_array, '0', ctx->mbox->file->keyword_count);
	memset(keyword_mask, 1, ctx->mbox->file->keyword_count);

	first_flag_offset = sizeof(struct dbox_mail_header);
	ret = dbox_sync_write_mask(ctx, sync_rec, first_flag_offset,
				   ctx->mbox->file->keyword_count,
				   keyword_array, keyword_mask);
	t_pop();
	return ret;
}

static int
dbox_sync_file_add_keywords(struct dbox_sync_context *ctx,
			    const struct dbox_sync_file_entry *entry,
			    unsigned int i)
{
	array_t ARRAY_DEFINE(keywords, struct seq_range);
	const struct dbox_sync_rec *sync_recs;
	const struct seq_range *range;
	unsigned int count, file_idx, keyword_idx;
	int ret = 0;

	if (dbox_file_seek(ctx->mbox, entry->file_seq, 0) <= 0)
		return -1;

	/* Get a list of all new keywords. Using seq_range is the easiest
	   way to do this and should be pretty fast too. */
	t_push();
	ARRAY_CREATE(&keywords, pool_datastack_create(), struct seq_range, 16);
	sync_recs = array_get(&entry->sync_recs, &count);
	for (; i < count; i++) {
		if (sync_recs[i].type != MAIL_INDEX_SYNC_TYPE_KEYWORD_ADD)
			continue;

		/* check if it's already in the file */
                keyword_idx = sync_recs[i].value.keyword_idx;
		if (dbox_file_lookup_keyword(ctx->mbox, ctx->mbox->file,
					     keyword_idx, &file_idx))
			continue;

		/* add it. if it already exists, it's handled internally. */
		seq_range_array_add(&keywords, 0, keyword_idx);
	}

	/* now, write them to file */
	range = array_get(&keywords, &count);
	if (count > 0) {
		ret = dbox_file_append_keywords(ctx->mbox, ctx->mbox->file,
						range, count);
	}

	t_pop();
	return ret;
}

static int dbox_sync_file(struct dbox_sync_context *ctx,
                          const struct dbox_sync_file_entry *entry)
{
	const struct dbox_sync_rec *sync_recs;
	unsigned int i, count;
	bool first_keyword = TRUE;
	int ret;

	sync_recs = array_get(&entry->sync_recs, &count);
	for (i = 0; i < count; i++) {
		switch (sync_recs[i].type) {
		case MAIL_INDEX_SYNC_TYPE_EXPUNGE:
			ret = dbox_sync_expunge(ctx, entry, i);
			if (ret > 0) {
				/* handled expunging by copying the file.
				   while at it, also wrote all the other sync
				   changes to the file. */
				return 0;
			}
			if (ret < 0)
				return -1;
			/* handled expunging by writing expunge flags */
			break;
		case MAIL_INDEX_SYNC_TYPE_FLAGS:
			if (dbox_sync_update_flags(ctx, &sync_recs[i]) < 0)
				return -1;
			break;
		case MAIL_INDEX_SYNC_TYPE_KEYWORD_ADD:
			if (first_keyword) {
				/* add all new keywords in one go */
				first_keyword = FALSE;
				if (dbox_sync_file_add_keywords(ctx, entry,
								i) < 0)
					return -1;
			}
			if (dbox_sync_update_keyword(ctx, &sync_recs[i],
						     TRUE) < 0)
				return -1;
			break;
		case MAIL_INDEX_SYNC_TYPE_KEYWORD_REMOVE:
			if (dbox_sync_update_keyword(ctx, &sync_recs[i],
						     FALSE) < 0)
				return -1;
			break;
		case MAIL_INDEX_SYNC_TYPE_KEYWORD_RESET:
			if (dbox_sync_reset_keyword(ctx, &sync_recs[i]) < 0)
				return -1;
			break;
		case MAIL_INDEX_SYNC_TYPE_APPEND:
			i_unreached();
		}
	}
	return 0;
}

static int dbox_sync_index(struct dbox_sync_context *ctx)
{
	struct mail_index_sync_rec sync_rec;
        struct hash_iterate_context *iter;
	void *key, *value;
	int ret;

	/* read all changes and sort them to file_seq order */
	ctx->pool = pool_alloconly_create("dbox sync pool", 10240);
	ctx->syncs = hash_create(default_pool, ctx->pool, 0, NULL, NULL);
	for (;;) {
		ret = mail_index_sync_next(ctx->index_sync_ctx, &sync_rec);
		if (ret <= 0) {
			if (ret < 0)
				mail_storage_set_index_error(&ctx->mbox->ibox);
			break;
		}
		if (dbox_sync_add(ctx, &sync_rec) < 0) {
			ret = -1;
			break;
		}
	}

	iter = hash_iterate_init(ctx->syncs);
	while (hash_iterate(iter, &key, &value)) {
                const struct dbox_sync_file_entry *entry = value;

		if (dbox_sync_file(ctx, entry) < 0) {
			ret = -1;
			break;
		}
	}
	hash_iterate_deinit(iter);

	hash_destroy(ctx->syncs);
	pool_unref(ctx->pool);

	return ret;
}

int dbox_sync(struct dbox_mailbox *mbox, bool force)
{
	struct dbox_sync_context ctx;
	const struct mail_index_header *hdr;
	uint32_t seq, uid_validity, next_uid;
	uoff_t offset;
	time_t mtime;
	int ret;

	memset(&ctx, 0, sizeof(ctx));
	ctx.mbox = mbox;

	/* always start index syncing before uidlist, so we don't get
	   deadlocks */
	ret = mail_index_sync_begin(mbox->ibox.index, &ctx.index_sync_ctx,
				    &ctx.sync_view, (uint32_t)-1, (uoff_t)-1,
				    !mbox->ibox.keep_recent, TRUE);
	if (ret <= 0) {
		if (ret < 0)
			mail_storage_set_index_error(&mbox->ibox);
		return ret;
	}
	if (dbox_uidlist_sync_init(mbox->uidlist, &ctx.uidlist_sync_ctx,
				   &mtime) < 0) {
		mail_index_sync_rollback(&ctx.index_sync_ctx);
		return -1;
	}

	ctx.trans = mail_index_transaction_begin(ctx.sync_view, FALSE, TRUE);

	hdr = mail_index_get_header(ctx.sync_view);
	if ((uint32_t)mtime != hdr->sync_stamp) {
		/* indexes aren't synced. we'll do a full sync. */
		force = TRUE;
	}

	if (force)
		ret = dbox_sync_full(&ctx);
	else
		ret = dbox_sync_index(&ctx);

	if (ret < 0) {
		mail_index_sync_rollback(&ctx.index_sync_ctx);
		dbox_uidlist_sync_rollback(ctx.uidlist_sync_ctx);
		return -1;
	}

	uid_validity = dbox_uidlist_sync_get_uid_validity(ctx.uidlist_sync_ctx);
	next_uid = dbox_uidlist_sync_get_next_uid(ctx.uidlist_sync_ctx);

	hdr = mail_index_get_header(ctx.sync_view);
	if (hdr->uid_validity != uid_validity) {
		mail_index_update_header(ctx.trans,
			offsetof(struct mail_index_header, uid_validity),
			&uid_validity, sizeof(uid_validity), TRUE);
	}
	if (hdr->next_uid != next_uid) {
		i_assert(next_uid > hdr->next_uid ||
			 hdr->uid_validity != uid_validity);
		mail_index_update_header(ctx.trans,
			offsetof(struct mail_index_header, next_uid),
			&next_uid, sizeof(next_uid), FALSE);
	}

	if (dbox_uidlist_sync_commit(ctx.uidlist_sync_ctx, &mtime) < 0) {
		mail_index_sync_rollback(&ctx.index_sync_ctx);
		return -1;
	}

	if ((uint32_t)mtime != hdr->sync_stamp) {
		uint32_t sync_stamp = mtime;

		mail_index_update_header(ctx.trans,
			offsetof(struct mail_index_header, sync_stamp),
			&sync_stamp, sizeof(sync_stamp), TRUE);
	}

	if (mail_index_transaction_commit(&ctx.trans, &seq, &offset) < 0) {
		mail_storage_set_index_error(&mbox->ibox);
		mail_index_sync_rollback(&ctx.index_sync_ctx);
		return -1;
	}

	if (force) {
		mail_index_sync_rollback(&ctx.index_sync_ctx);
		/* now that indexes are ok, sync changes from the index */
		return dbox_sync(mbox, FALSE);
	} else {
		if (mail_index_sync_commit(&ctx.index_sync_ctx) < 0) {
			mail_storage_set_index_error(&mbox->ibox);
			return -1;
		}
	}
	return 0;
}

struct mailbox_sync_context *
dbox_storage_sync_init(struct mailbox *box, enum mailbox_sync_flags flags)
{
	struct dbox_mailbox *mbox = (struct dbox_mailbox *)box;
	int ret = 0;

	if ((flags & MAILBOX_SYNC_FLAG_FAST) == 0 ||
	    mbox->ibox.sync_last_check + MAILBOX_FULL_SYNC_INTERVAL <=
	    ioloop_time)
		ret = dbox_sync(mbox, FALSE);

	return index_mailbox_sync_init(box, flags, ret < 0);
}

int dbox_sync_if_changed(struct dbox_mailbox *mbox)
{
	const struct mail_index_header *hdr;
	time_t mtime;

	hdr = mail_index_get_header(mbox->ibox.view);
	if (hdr->sync_stamp == 0)
		return 1;

	if (dbox_uidlist_get_mtime(mbox->uidlist, &mtime) < 0)
		return -1;

	return (uint32_t)mtime == hdr->sync_stamp;
}
