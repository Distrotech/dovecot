/* Copyright (C) 2004 Timo Sirainen */

#include "lib.h"
#include "buffer.h"
#include "file-set-size.h"
#include "mmap-util.h"
#include "mail-index-view-private.h"
#include "mail-index-sync-private.h"
#include "mail-transaction-log.h"

struct mail_index_update_ctx {
	struct mail_index *index;
	struct mail_index_header hdr;
	struct mail_transaction_log_view *log_view;
};

void mail_index_header_update_counts(struct mail_index_header *hdr,
				     uint8_t old_flags, uint8_t new_flags)
{
	if (((old_flags ^ new_flags) & MAIL_SEEN) != 0) {
		/* different seen-flag */
		if ((old_flags & MAIL_SEEN) == 0)
			hdr->seen_messages_count++;
		else
			hdr->seen_messages_count--;
	}

	if (((old_flags ^ new_flags) & MAIL_DELETED) != 0) {
		/* different deleted-flag */
		if ((old_flags & MAIL_DELETED) == 0)
			hdr->deleted_messages_count++;
		else
			hdr->deleted_messages_count--;
	}
}

void mail_index_header_update_lowwaters(struct mail_index_header *hdr,
					const struct mail_index_record *rec)
{
	if ((rec->flags & MAIL_RECENT) != 0 &&
	    rec->uid < hdr->first_recent_uid_lowwater)
		hdr->first_recent_uid_lowwater = rec->uid;
	if ((rec->flags & MAIL_SEEN) == 0 &&
	    rec->uid < hdr->first_unseen_uid_lowwater)
		hdr->first_unseen_uid_lowwater = rec->uid;
	if ((rec->flags & MAIL_DELETED) != 0 &&
	    rec->uid < hdr->first_deleted_uid_lowwater)
		hdr->first_deleted_uid_lowwater = rec->uid;
}

static void mail_index_sync_update_expunges(struct mail_index_update_ctx *ctx,
					    uint32_t seq1, uint32_t seq2)
{
	struct mail_index_record *rec;

	rec = &ctx->index->map->records[seq1-1];
	for (; seq1 <= seq2; seq1++, rec++)
		mail_index_header_update_counts(&ctx->hdr, rec->flags, 0);
}

static void mail_index_sync_update_flags(struct mail_index_update_ctx *ctx,
					 struct mail_index_sync_rec *syncrec)
{
	struct mail_index_record *rec, *end;
	uint8_t flag_mask, old_flags;
	keywords_mask_t keyword_mask;
	int i, update_keywords;

	update_keywords = FALSE;
	for (i = 0; i < INDEX_KEYWORDS_BYTE_COUNT; i++) {
		if (syncrec->add_keywords[i] != 0)
			update_keywords = TRUE;
		if (syncrec->remove_keywords[i] != 0)
			update_keywords = TRUE;
		keyword_mask[i] = ~syncrec->remove_keywords[i];
	}

	flag_mask = ~syncrec->remove_flags;
	rec = &ctx->index->map->records[syncrec->seq1-1];
	end = rec + (syncrec->seq2 - syncrec->seq1) + 1;
	for (; rec != end; rec++) {
		old_flags = rec->flags;
		rec->flags = (rec->flags & flag_mask) | syncrec->add_flags;
		if (update_keywords) {
			for (i = 0; i < INDEX_KEYWORDS_BYTE_COUNT; i++) {
				rec->keywords[i] =
					(rec->keywords[i] & keyword_mask[i]) |
					syncrec->add_keywords[i];
			}
		}

		mail_index_header_update_counts(&ctx->hdr,
						old_flags, rec->flags);
                mail_index_header_update_lowwaters(&ctx->hdr, rec);
	}
}

static int mail_index_grow(struct mail_index *index, unsigned int count)
{
	struct mail_index_map *map = index->map;
	unsigned int records_count;
	size_t size;

	if (MAIL_INDEX_MAP_IS_IN_MEMORY(map)) {
		(void)buffer_append_space_unsafe(map->buffer,
			count * sizeof(struct mail_index_record));
		map->records = buffer_get_modifyable_data(map->buffer, NULL);
		return 0;
	}

	size = map->hdr->header_size +
		(map->records_count + count) * sizeof(struct mail_index_record);
	if (size <= map->mmap_size)
		return 0;

	/* when we grow fast, do it exponentially */
	if (count < index->last_grow_count)
		count = index->last_grow_count;
	if (count < MAIL_INDEX_MAX_POWER_GROW)
		count = nearest_power(count);
	index->last_grow_count = count;

	size = map->hdr->header_size +
		(map->records_count + count) * sizeof(struct mail_index_record);
	if (file_set_size(index->fd, (off_t)size) < 0)
		return mail_index_set_syscall_error(index, "file_set_size()");

	records_count = map->records_count;

	if (mail_index_map(index, TRUE) <= 0)
		return -1;

	i_assert(map->mmap_size >= size);
	map->records_count = records_count;
	return 0;
}

static int mail_index_sync_appends(struct mail_index_update_ctx *ctx,
				   const struct mail_index_record *appends,
				   unsigned int count)
{
	struct mail_index_map *map = ctx->index->map;
	unsigned int i;
	uint32_t next_uid;

	if (mail_index_grow(ctx->index, count) < 0)
		return -1;

	next_uid = ctx->hdr.next_uid;
	for (i = 0; i < count; i++) {
		mail_index_header_update_counts(&ctx->hdr, 0, appends[i].flags);
                mail_index_header_update_lowwaters(&ctx->hdr, &appends[i]);

		if (appends[i].uid < next_uid) {
			mail_transaction_log_view_set_corrupted(ctx->log_view,
				"Append with UID %u, but next_uid = %u",
				appends[i].uid, next_uid);
			return -1;
		}
		next_uid = appends[i].uid+1;
	}
	ctx->hdr.next_uid = next_uid;

	memcpy(map->records + map->records_count, appends,
	       count * sizeof(*appends));
	map->records_count += count;
	return 0;
}

int mail_index_sync_update_index(struct mail_index_sync_ctx *sync_ctx,
				 uint32_t sync_stamp, uint64_t sync_size)
{
	struct mail_index *index = sync_ctx->index;
	struct mail_index_map *map;
        struct mail_index_update_ctx ctx;
	struct mail_index_sync_rec rec;
	const struct mail_index_record *appends;
	unsigned int append_count;
	uint32_t count, file_seq, src_idx, dest_idx, dirty_flag;
	uoff_t file_offset;
	unsigned int lock_id;
	int ret, changed;

	/* rewind */
	sync_ctx->update_idx = sync_ctx->expunge_idx = 0;
	sync_ctx->sync_appends =
		buffer_get_used_size(sync_ctx->appends_buf) != 0;

	changed = mail_index_sync_have_more(sync_ctx);

	memset(&ctx, 0, sizeof(ctx));
	ctx.index = index;
	ctx.hdr = *index->hdr;
	ctx.log_view = sync_ctx->view->log_view;

	dirty_flag = sync_ctx->have_dirty ? MAIL_INDEX_HDR_FLAG_HAVE_DIRTY : 0;
	if ((ctx.hdr.flags & MAIL_INDEX_HDR_FLAG_HAVE_DIRTY) != dirty_flag) {
		ctx.hdr.flags ^= MAIL_INDEX_HDR_FLAG_HAVE_DIRTY;
		changed = TRUE;
	}

	/* see if we need to update sync headers */
	if (ctx.hdr.sync_stamp != sync_stamp && sync_stamp != 0) {
		ctx.hdr.sync_stamp = sync_stamp;
		changed = TRUE;
	}
	if (ctx.hdr.sync_size != sync_size && sync_size != 0) {
		ctx.hdr.sync_size = sync_size;
		changed = TRUE;
	}

	if (!changed) {
		/* nothing to sync */
		return 0;
	}

	if (mail_index_lock_exclusive(index, &lock_id) < 0)
		return -1;

	map = index->map;
	if (MAIL_INDEX_MAP_IS_IN_MEMORY(map))
		map->write_to_disk = TRUE;

	src_idx = dest_idx = 0;
	append_count = 0; appends = NULL;
	while (mail_index_sync_next(sync_ctx, &rec) > 0) {
		switch (rec.type) {
		case MAIL_INDEX_SYNC_TYPE_APPEND:
			i_assert(appends == NULL);
			append_count = rec.seq2 - rec.seq1 + 1;
			appends = rec.appends;
			break;
		case MAIL_INDEX_SYNC_TYPE_EXPUNGE:
			if (src_idx == 0) {
				/* expunges have to be atomic. so we'll have
				   to copy the mapping, do the changes there
				   and then finally replace the whole index
				   file. to avoid extra disk I/O we copy the
				   index into memory rather than to temporary
				   file */
				map = mail_index_map_to_memory(map);
				mail_index_unmap(index, index->map);
				index->map = map;
				index->hdr = map->hdr;
				map->write_to_disk = TRUE;

				dest_idx = rec.seq1-1;
			} else {
				count = (rec.seq1-1) - src_idx;
				memmove(map->records + dest_idx,
					map->records + src_idx,
					count * sizeof(*map->records));
				dest_idx += count;
			}

			mail_index_sync_update_expunges(&ctx, rec.seq1,
							rec.seq2);
			src_idx = rec.seq2;
			break;
		case MAIL_INDEX_SYNC_TYPE_FLAGS:
			mail_index_sync_update_flags(&ctx, &rec);
			break;
		}
	}

	if (src_idx != 0) {
		count = map->records_count - src_idx;
		memmove(map->records + dest_idx,
			map->records + src_idx,
			count * sizeof(*map->records));
		dest_idx += count;

		map->records_count = dest_idx;
	}

	ret = 0;
	if (append_count > 0)
		ret = mail_index_sync_appends(&ctx, appends, append_count);

	mail_transaction_log_get_head(index->log, &file_seq, &file_offset);

	ctx.hdr.messages_count = map->records_count;
	ctx.hdr.log_file_seq = file_seq;
	ctx.hdr.log_file_offset = file_offset;

	if (!MAIL_INDEX_MAP_IS_IN_MEMORY(map)) {
		map->mmap_used_size = index->hdr->header_size +
			map->records_count * sizeof(struct mail_index_record);

		memcpy(map->mmap_base, &ctx.hdr, sizeof(ctx.hdr));
		if (msync(map->mmap_base, map->mmap_used_size, MS_SYNC) < 0) {
			mail_index_set_syscall_error(index, "msync()");
			ret = -1;
		}
	} else {
		map->hdr_copy = ctx.hdr;
		map->hdr = &map->hdr_copy;
	}

	mail_index_unlock(index, lock_id);
	return ret;
}
