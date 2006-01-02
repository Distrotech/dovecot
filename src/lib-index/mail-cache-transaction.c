/* Copyright (C) 2003-2004 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "buffer.h"
#include "file-cache.h"
#include "file-set-size.h"
#include "read-full.h"
#include "write-full.h"
#include "mail-cache-private.h"
#include "mail-index-transaction-private.h"

#include <stddef.h>
#include <sys/stat.h>

#define MAIL_CACHE_WRITE_BUFFER 32768

struct mail_cache_transaction_ctx {
	struct mail_cache *cache;
	struct mail_cache_view *view;
	struct mail_index_transaction *trans;

	uint32_t cache_file_seq;

	buffer_t *cache_data;
	array_t ARRAY_DEFINE(cache_data_seq, uint32_t);
	uint32_t prev_seq;
	size_t prev_pos;

        buffer_t *reservations;
	uint32_t reserved_space_offset, reserved_space;
	uint32_t last_grow_size;

	unsigned int changes:1;
};

static int mail_cache_link_unlocked(struct mail_cache *cache,
				    uint32_t old_offset, uint32_t new_offset);

struct mail_cache_transaction_ctx *
mail_cache_get_transaction(struct mail_cache_view *view,
			   struct mail_index_transaction *t)
{
	struct mail_cache_transaction_ctx *ctx;

	if (t->cache_trans_ctx != NULL)
		return t->cache_trans_ctx;

	ctx = i_new(struct mail_cache_transaction_ctx, 1);
	ctx->cache = view->cache;
	ctx->view = view;
	ctx->trans = t;
	ctx->reservations = buffer_create_dynamic(system_pool, 256);

	if (!MAIL_CACHE_IS_UNUSABLE(ctx->cache))
		ctx->cache_file_seq = ctx->cache->hdr->file_seq;

	i_assert(view->transaction == NULL);
	view->transaction = ctx;
	view->trans_view = mail_index_transaction_open_updated_view(t);

	t->cache_trans_ctx = ctx;
	return ctx;
}

static void mail_cache_transaction_reset(struct mail_cache_transaction_ctx *ctx)
{
	ctx->cache_file_seq = ctx->cache->hdr->file_seq;

	if (ctx->cache_data)
		buffer_set_used_size(ctx->cache_data, 0);
	if (array_is_created(&ctx->cache_data_seq))
		array_clear(&ctx->cache_data_seq);
	ctx->prev_seq = 0;
	ctx->prev_pos = 0;

	buffer_set_used_size(ctx->reservations, 0);
	ctx->reserved_space_offset = 0;
	ctx->reserved_space = 0;
	ctx->last_grow_size = 0;

	ctx->changes = FALSE;
}

static void mail_cache_transaction_free(struct mail_cache_transaction_ctx *ctx)
{
	ctx->view->transaction = NULL;
	ctx->view->trans_seq1 = ctx->view->trans_seq2 = 0;

	if (ctx->cache_data != NULL)
		buffer_free(ctx->cache_data);
	if (array_is_created(&ctx->cache_data_seq))
		array_free(&ctx->cache_data_seq);
	buffer_free(ctx->reservations);
	i_free(ctx);
}

static int mail_cache_transaction_lock(struct mail_cache_transaction_ctx *ctx)
{
	int ret;

	if ((ret = mail_cache_lock(ctx->cache)) <= 0)
		return ret;

	if (ctx->cache_file_seq != ctx->cache->hdr->file_seq)
		mail_cache_transaction_reset(ctx);
	return 1;
}

static int mail_cache_grow_file(struct mail_cache *cache, size_t size)
{
	struct stat st;
	uoff_t new_fsize, grow_size;

	i_assert(cache->locked);

	/* grow the file */
	new_fsize = cache->hdr_copy.used_file_size + size;
	grow_size = new_fsize / 100 * MAIL_CACHE_GROW_PERCENTAGE;
	if (grow_size < 16384)
		grow_size = 16384;
	new_fsize += grow_size;
	new_fsize &= ~1023;

	if (fstat(cache->fd, &st) < 0) {
		mail_cache_set_syscall_error(cache, "fstat()");
		return -1;
	}

	if ((uoff_t)st.st_size < new_fsize) {
		if (file_set_size(cache->fd, new_fsize) < 0) {
			mail_cache_set_syscall_error(cache, "file_set_size()");
			return -1;
		}
	}
	return 0;
}

static int mail_cache_unlink_hole(struct mail_cache *cache, size_t size,
				  struct mail_cache_hole_header *hole_r)
{
	struct mail_cache_header *hdr = &cache->hdr_copy;
	struct mail_cache_hole_header hole;
	uint32_t offset, prev_offset;

	i_assert(cache->locked);

	offset = hdr->hole_offset; prev_offset = 0;
	while (offset != 0) {
		if (pread_full(cache->fd, &hole, sizeof(hole), offset) <= 0) {
			mail_cache_set_syscall_error(cache, "pread_full()");
			return FALSE;
		}

		if (hole.magic != MAIL_CACHE_HOLE_HEADER_MAGIC) {
			mail_cache_set_corrupted(cache,
				"Invalid magic in hole header");
			return FALSE;
		}

		if (hole.size >= size)
			break;

		prev_offset = offset;
		offset = hole.next_offset;
	}
	if (offset == 0)
		return FALSE;

	if (prev_offset == 0)
		hdr->hole_offset = hole.next_offset;
	else {
		if (mail_cache_write(cache, &hole.next_offset,
				     sizeof(hole.next_offset), prev_offset) < 0)
			return FALSE;
	}
	hdr->deleted_space -= hole.size;
	cache->hdr_modified = TRUE;

	hole_r->next_offset = offset;
	hole_r->size = hole.size;
	return TRUE;
}

static void
mail_cache_transaction_add_reservation(struct mail_cache_transaction_ctx *ctx,
				       uint32_t offset, uint32_t size)
{
	ctx->reserved_space_offset = offset;
	ctx->reserved_space = size;

	buffer_append(ctx->reservations, &offset, sizeof(offset));
	buffer_append(ctx->reservations, &size, sizeof(size));
}

static int
mail_cache_transaction_reserve_more(struct mail_cache_transaction_ctx *ctx,
				    size_t block_size, int commit)
{
	struct mail_cache *cache = ctx->cache;
	struct mail_cache_header *hdr = &cache->hdr_copy;
	struct mail_cache_hole_header hole;
	uint32_t *buf;
	size_t size;

	i_assert(cache->locked);

	if (mail_cache_unlink_hole(cache, block_size, &hole)) {
		/* found a large enough hole. */
		mail_cache_transaction_add_reservation(ctx, hole.next_offset,
						       hole.size);
		return 0;
	}

	if (MAIL_CACHE_IS_UNUSABLE(cache)) {
		/* mail_cache_unlink_hole() could have noticed corruption */
		return -1;
	}

	if ((uint32_t)-1 - hdr->used_file_size < block_size) {
		mail_index_set_error(cache->index, "Cache file too large: %s",
				     cache->filepath);
		return -1;
	}

	if (!commit && block_size < MAIL_CACHE_MAX_RESERVED_BLOCK_SIZE) {
		/* allocate some more space than we need */
		size_t new_block_size = (block_size + ctx->last_grow_size) * 2;
		if (new_block_size > MAIL_CACHE_MAX_RESERVED_BLOCK_SIZE)
			new_block_size = MAIL_CACHE_MAX_RESERVED_BLOCK_SIZE;

		if ((uint32_t)-1 - hdr->used_file_size >= new_block_size) {
			block_size = new_block_size;
			ctx->last_grow_size = new_block_size;
		}
	}

	if (mail_cache_grow_file(ctx->cache, block_size) < 0)
		return -1;

	if (ctx->reserved_space_offset + ctx->reserved_space ==
	    hdr->used_file_size) {
		/* we can simply grow it */

		/* grow reservation. it's probably the last one in the buffer,
		   but it's not guarateed because we might have used holes
		   as well */
		buf = buffer_get_modifyable_data(ctx->reservations, &size);
		size /= sizeof(uint32_t);

		do {
			i_assert(size >= 2);
			size -= 2;
		} while (buf[size] + buf[size+1] != hdr->used_file_size);

		buf[size+1] += block_size;
		ctx->reserved_space += block_size;
	} else {
		mail_cache_transaction_add_reservation(ctx, hdr->used_file_size,
						       block_size);
	}

	cache->hdr_modified = TRUE;
	hdr->used_file_size = ctx->reserved_space_offset + ctx->reserved_space;
	return 0;
}

static void
mail_cache_free_space(struct mail_cache *cache, uint32_t offset, uint32_t size)
{
	struct mail_cache_hole_header hole;

	i_assert(cache->locked);

	if (MAIL_CACHE_IS_UNUSABLE(cache))
		return;

	if (offset + size == cache->hdr_copy.used_file_size) {
		/* we can just set used_file_size back */
		cache->hdr_modified = TRUE;
		cache->hdr_copy.used_file_size = offset;
	} else if (size >= MAIL_CACHE_MIN_HOLE_SIZE) {
		/* set it up as a hole */
		hole.next_offset = cache->hdr_copy.hole_offset;
		hole.size = size;
		hole.magic = MAIL_CACHE_HOLE_HEADER_MAGIC;

		if (mail_cache_write(cache, &hole, sizeof(hole), offset) < 0)
			return;

		cache->hdr_copy.deleted_space += size;
		cache->hdr_copy.hole_offset = offset;
		cache->hdr_modified = TRUE;
	}
}

static int
mail_cache_transaction_free_space(struct mail_cache_transaction_ctx *ctx)
{
	int locked = ctx->cache->locked;

	if (ctx->reserved_space == 0)
		return 0;

	if (!locked) {
		if (mail_cache_transaction_lock(ctx) <= 0)
			return 0;
	}

	/* check again - locking might have reopened the cache file */
	if (ctx->reserved_space != 0) {
		i_assert(ctx->cache_file_seq == ctx->cache->hdr->file_seq);
		mail_cache_free_space(ctx->cache, ctx->reserved_space_offset,
				      ctx->reserved_space);
		ctx->reserved_space_offset = 0;
                ctx->reserved_space = 0;
	}

	if (!locked) {
		if (mail_cache_unlock(ctx->cache) < 0)
			return -1;
	}
	return 0;
}

static int
mail_cache_transaction_get_space(struct mail_cache_transaction_ctx *ctx,
				 size_t min_size, size_t max_size,
				 uint32_t *offset_r, size_t *available_space_r,
				 int commit)
{
	int locked = ctx->cache->locked;
	uint32_t cache_file_seq;
	size_t size;
	int ret;

	i_assert((min_size & 3) == 0);
	i_assert((max_size & 3) == 0);

	if (min_size > ctx->reserved_space) {
		/* not enough preallocated space in transaction, get more */
		cache_file_seq = ctx->cache_file_seq;
		if (!locked) {
			if ((ret = mail_cache_transaction_lock(ctx)) <= 0)
				return ret;
		}
		ret = mail_cache_transaction_reserve_more(ctx, max_size,
							  commit);
		if (!locked) {
			if (mail_cache_unlock(ctx->cache) < 0)
				return -1;
		}

		if (ret < 0)
			return -1;

		if (cache_file_seq != ctx->cache_file_seq) {
			/* cache file reopened - need to abort */
			return 0;
		}

		size = max_size;
	} else {
		size = I_MIN(max_size, ctx->reserved_space);
	}

	*offset_r = ctx->reserved_space_offset;
	ctx->reserved_space_offset += size;
	ctx->reserved_space -= size;
	if (available_space_r != NULL)
		*available_space_r = size;
	i_assert((size & 3) == 0);

	if (size == max_size && commit) {
		/* final commit - see if we can free the rest of the
		   reserved space */
		if (mail_cache_transaction_free_space(ctx) < 0)
			return -1;
	}

	i_assert(size >= min_size);
	return 1;
}

static int
mail_cache_transaction_update_index(struct mail_cache_transaction_ctx *ctx,
				    const struct mail_cache_record *rec,
				    const uint32_t *seq, uint32_t *seq_idx,
				    uint32_t seq_limit, uint32_t write_offset,
				    uint32_t *size_r)
{
	struct mail_cache *cache = ctx->cache;
	uint32_t i, old_offset, orig_write_offset;

	/* write the cache_offsets to index file. records' prev_offset
	   is updated to point to old cache record when index is being
	   synced. */
	orig_write_offset = write_offset;
	for (i = *seq_idx; i < seq_limit; i++) {
		mail_index_update_ext(ctx->trans, seq[i], cache->ext_id,
				      &write_offset, &old_offset);
		if (old_offset != 0) {
			/* we added records for this message multiple
			   times in this same uncommitted transaction.
			   only the new one will be written to
			   transaction log, we need to do the linking
			   ourself here. */
			if (old_offset > write_offset) {
				if (mail_cache_link_unlocked(cache, old_offset,
							     write_offset) < 0)
					return -1;
			} else {
				/* if we're combining multiple transactions,
				   make sure the one with the smallest offset
				   is written into index. this is required for
				   non-file-mmaped cache to work properly. */
				mail_index_update_ext(ctx->trans, seq[i],
						      cache->ext_id,
						      &old_offset, NULL);
				if (mail_cache_link_unlocked(cache,
							     write_offset,
							     old_offset) < 0)
					return -1;
			}
		}

		write_offset += rec->size;
		rec = CONST_PTR_OFFSET(rec, rec->size);
	}

	*seq_idx = i;
	*size_r = write_offset - orig_write_offset;
	return 0;
}

static int
mail_cache_transaction_flush(struct mail_cache_transaction_ctx *ctx)
{
	struct mail_cache *cache = ctx->cache;
	const struct mail_cache_record *rec, *tmp_rec;
	const uint32_t *seq;
	uint32_t write_offset, write_size, rec_pos, seq_idx, seq_limit;
	size_t size, max_size;
	unsigned int seq_count;
	int ret, commit;

	if (MAIL_CACHE_IS_UNUSABLE(cache))
		return -1;

	commit = ctx->prev_seq == 0;
	if (commit) {
		/* committing, remove the last dummy record */
		buffer_set_used_size(ctx->cache_data, ctx->prev_pos);
	}

	if (ctx->cache_file_seq != ctx->cache->hdr->file_seq) {
		/* cache file reopened - need to abort */
		mail_cache_transaction_reset(ctx);
		return 0;
	}

	rec = buffer_get_data(ctx->cache_data, &size);
	i_assert(ctx->prev_pos <= size);

	seq = array_get(&ctx->cache_data_seq, &seq_count);
	seq_limit = 0;

	for (seq_idx = 0, rec_pos = 0; rec_pos < ctx->prev_pos;) {
		max_size = ctx->prev_pos - rec_pos;

		ret = mail_cache_transaction_get_space(ctx, rec->size,
						       max_size, &write_offset,
						       &max_size, commit);
		if (ret <= 0) {
			/* nothing to write / error / cache file reopened */
			return ret;
		}

		if (rec_pos + max_size < ctx->prev_pos) {
			/* see how much we can really write there */
			tmp_rec = rec;
			for (size = 0; size + tmp_rec->size <= max_size; ) {
				seq_limit++;
				size += tmp_rec->size;
				tmp_rec = CONST_PTR_OFFSET(tmp_rec,
							   tmp_rec->size);
			}
			max_size = size;
		} else {
			seq_limit = seq_count;
		}

		/* write it to file */
		i_assert(ctx->cache_file_seq == cache->hdr->file_seq);
		if (mail_cache_write(cache, rec, max_size, write_offset) < 0)
			return -1;

		if (mail_cache_transaction_update_index(ctx, rec, seq,
							&seq_idx, seq_limit,
							write_offset,
							&write_size) < 0)
			return -1;

		rec_pos += write_size;
		rec = CONST_PTR_OFFSET(rec, write_size);
	}

	/* drop the written data from buffer */
	buffer_copy(ctx->cache_data, 0,
		    ctx->cache_data, ctx->prev_pos, (size_t)-1);
	buffer_set_used_size(ctx->cache_data,
			     buffer_get_used_size(ctx->cache_data) -
			     ctx->prev_pos);
	ctx->prev_pos = 0;

	array_clear(&ctx->cache_data_seq);
	return 1;
}

static void
mail_cache_transaction_switch_seq(struct mail_cache_transaction_ctx *ctx)
{
	struct mail_cache_record *rec, new_rec;
	void *data;
	size_t size;

	if (ctx->prev_seq != 0) {
		/* fix record size */
		data = buffer_get_modifyable_data(ctx->cache_data, &size);
		rec = PTR_OFFSET(data, ctx->prev_pos);
		rec->size = size - ctx->prev_pos;
		i_assert(rec->size != 0);

		array_append(&ctx->cache_data_seq, &ctx->prev_seq, 1);
		ctx->prev_pos = size;
	} else if (ctx->cache_data == NULL) {
		ctx->cache_data =
			buffer_create_dynamic(default_pool,
					      MAIL_CACHE_WRITE_BUFFER);
		ARRAY_CREATE(&ctx->cache_data_seq, default_pool, uint32_t, 64);
	}

	memset(&new_rec, 0, sizeof(new_rec));
	buffer_append(ctx->cache_data, &new_rec, sizeof(new_rec));

	ctx->prev_seq = 0;
	ctx->changes = TRUE;
}

int mail_cache_transaction_commit(struct mail_cache_transaction_ctx *ctx)
{
	struct mail_cache *cache = ctx->cache;
	int ret = 0;

	if (!ctx->changes || MAIL_CACHE_IS_UNUSABLE(cache)) {
		mail_cache_transaction_free(ctx);
		return 0;
	}

	if (mail_cache_transaction_lock(ctx) <= 0) {
		mail_cache_transaction_rollback(ctx);
		return -1;
	}

	if (ctx->prev_seq != 0)
                mail_cache_transaction_switch_seq(ctx);

	if (mail_cache_transaction_flush(ctx) < 0)
		ret = -1;

	/* Here would be a good place to do fdatasync() to make sure
	   everything is written before offsets are updated to index.
	   However it slows down I/O unneededly and we're pretty good at
	   catching and fixing cache corruption, so we no longer do it. */

	if (mail_cache_unlock(cache) < 0)
		ret = -1;
	mail_cache_transaction_free(ctx);
	return ret;
}

void mail_cache_transaction_rollback(struct mail_cache_transaction_ctx *ctx)
{
	struct mail_cache *cache = ctx->cache;
	const uint32_t *buf;
	size_t size;

	if ((ctx->reserved_space > 0 || ctx->reservations->used > 0) &&
	    !MAIL_CACHE_IS_UNUSABLE(cache)) {
		if (mail_cache_transaction_lock(ctx) > 0) {
			buf = buffer_get_data(ctx->reservations, &size);
			i_assert(size % sizeof(uint32_t)*2 == 0);
			size /= sizeof(*buf);

			if (size > 0) {
				/* free flushed data as well. do it from end to
				   beginning so we have a better chance of
				   updating used_file_size instead of adding
				   holes */
				do {
					size -= 2;
					mail_cache_free_space(ctx->cache,
							      buf[size],
							      buf[size+1]);
				} while (size > 0);
			}
			(void)mail_cache_unlock(cache);
		}
	}

	mail_cache_transaction_free(ctx);
}

static int mail_cache_header_add_field(struct mail_cache_transaction_ctx *ctx,
				       unsigned int field)
{
	struct mail_cache *cache = ctx->cache;
	buffer_t *buffer;
	const void *data;
	size_t size;
	uint32_t offset, hdr_offset;
	int ret = 0;

	if (mail_cache_transaction_lock(ctx) <= 0)
		return -1;

	/* re-read header to make sure we don't lose any fields. */
	if (mail_cache_header_fields_read(cache) < 0) {
		(void)mail_cache_unlock(cache);
		return -1;
	}

	if (ctx->cache->field_file_map[field] != (uint32_t)-1) {
		/* it was already added */
		if (mail_cache_unlock(cache) < 0)
			return -1;
		return 0;
	}

	t_push();
	buffer = buffer_create_dynamic(pool_datastack_create(), 256);
	mail_cache_header_fields_get(cache, buffer);
	data = buffer_get_data(buffer, &size);

	if (mail_cache_transaction_get_space(ctx, size, size,
					     &offset, NULL, TRUE) <= 0)
		ret = -1;
	else if (mail_cache_write(cache, data, size, offset) < 0)
		ret = -1;
	else if (fdatasync(cache->fd) < 0) {
		mail_cache_set_syscall_error(cache, "fdatasync()");
		ret = -1;
	} else if (mail_cache_header_fields_get_next_offset(cache,
							    &hdr_offset) < 0)
		ret = -1;
	else {
		/* after it's guaranteed to be in disk, update header offset */
		offset = mail_index_uint32_to_offset(offset);
		if (mail_cache_write(cache, &offset, sizeof(offset),
				     hdr_offset) < 0)
			ret = -1;
		else {
			/* we'll need to fix mappings. */
			if (mail_cache_header_fields_read(cache) < 0)
				ret = -1;
		}
	}
	t_pop();

	if (mail_cache_unlock(cache) < 0)
		ret = -1;
	return ret;
}

void mail_cache_add(struct mail_cache_transaction_ctx *ctx, uint32_t seq,
		    unsigned int field, const void *data, size_t data_size)
{
	uint32_t file_field, data_size32;
	unsigned int fixed_size;
	size_t full_size;

	i_assert(field < ctx->cache->fields_count);
	i_assert(data_size < (uint32_t)-1);

	if (ctx->cache->fields[field].field.decision ==
	    (MAIL_CACHE_DECISION_NO | MAIL_CACHE_DECISION_FORCED))
		return;

	file_field = ctx->cache->field_file_map[field];
	if (file_field == (uint32_t)-1) {
		/* we'll have to add this field to headers */
		if (mail_cache_header_add_field(ctx, field) < 0)
			return;

		file_field = ctx->cache->field_file_map[field];
		i_assert(file_field != (uint32_t)-1);
	}

	mail_cache_decision_add(ctx->view, seq, field);

	fixed_size = ctx->cache->fields[field].field.field_size;
	i_assert(fixed_size == (unsigned int)-1 || fixed_size == data_size);

	data_size32 = (uint32_t)data_size;

	if (ctx->prev_seq != seq) {
		mail_cache_transaction_switch_seq(ctx);
		ctx->prev_seq = seq;

		/* remember roughly what we have modified, so cache lookups can
		   look into transactions to see changes. */
		if (seq < ctx->view->trans_seq1 || ctx->view->trans_seq1 == 0)
			ctx->view->trans_seq1 = seq;
		if (seq > ctx->view->trans_seq2)
			ctx->view->trans_seq2 = seq;
	}

	full_size = (data_size + 3) & ~3;
	if (fixed_size == (unsigned int)-1)
		full_size += sizeof(data_size32);

	if (ctx->cache_data->used + full_size >
	    buffer_get_size(ctx->cache_data) && ctx->prev_pos > 0) {
		/* time to flush our buffer. if flushing fails because the
		   cache file had been compressed and was reopened, return
		   without adding the cached data since cache_data buffer
		   doesn't contain the cache_rec anymore. */
		if (mail_cache_transaction_flush(ctx) <= 0)
			return;
	}

	buffer_append(ctx->cache_data, &file_field, sizeof(file_field));
	if (fixed_size == (unsigned int)-1) {
		buffer_append(ctx->cache_data, &data_size32,
			      sizeof(data_size32));
	}

	buffer_append(ctx->cache_data, data, data_size);
	if ((data_size & 3) != 0)
                buffer_append_zero(ctx->cache_data, 4 - (data_size & 3));
}

static int mail_cache_link_unlocked(struct mail_cache *cache,
				    uint32_t old_offset, uint32_t new_offset)
{
	new_offset += offsetof(struct mail_cache_record, prev_offset);
	return mail_cache_write(cache, &old_offset, sizeof(old_offset),
				new_offset);
}

int mail_cache_link(struct mail_cache *cache, uint32_t old_offset,
		    uint32_t new_offset)
{
	i_assert(cache->locked);

	if (MAIL_CACHE_IS_UNUSABLE(cache))
		return -1;

	if (new_offset + sizeof(struct mail_cache_record) >
	    cache->hdr_copy.used_file_size) {
		mail_cache_set_corrupted(cache,
			"Cache record offset %u points outside file",
			new_offset);
		return -1;
	}

	if (mail_cache_link_unlocked(cache, old_offset, new_offset) < 0)
		return -1;

	cache->hdr_copy.continued_record_count++;
	cache->hdr_modified = TRUE;
	return 0;
}

static int find_offset(array_t *array, uint32_t offset)
{
	ARRAY_SET_TYPE(array, uint32_t);
	const uint32_t *offsets;
	unsigned int i, count;

	offsets = array_get(array, &count);
	for (i = 0; i < count; i++) {
		if (offsets[i] == offset)
			return TRUE;
	}
	return FALSE;
}

int mail_cache_delete(struct mail_cache *cache, uint32_t offset)
{
	const struct mail_cache_record *cache_rec;
	array_t ARRAY_DEFINE(tmp_offsets, uint32_t);

	i_assert(cache->locked);

	if (mail_cache_get_record(cache, offset, &cache_rec) < 0)
		return -1;
	if (cache_rec == NULL)
		return 0;

	/* we'll only update the deleted_space in header. we can't really
	   do any actual deleting as other processes might still be using
	   the data. also it's actually useful as some index views are still
	   able to ask cached data from messages that have already been
	   expunged. */
	t_push();
	ARRAY_CREATE(&tmp_offsets, pool_datastack_create(), uint32_t, 8);
	array_append(&tmp_offsets, &offset, 1);
	for (;;) {
		cache->hdr_copy.deleted_space += cache_rec->size;
		if (mail_cache_get_record(cache, cache_rec->prev_offset,
					  &cache_rec) < 0) {
			t_pop();
			return -1;
		}

		if (cache_rec == NULL)
			break;

		if (find_offset(&tmp_offsets, cache_rec->prev_offset)) {
			mail_cache_set_corrupted(cache,
						 "record list is circular");
			t_pop();
			return -1;
		}
		array_append(&tmp_offsets, &cache_rec->prev_offset, 1);
	}
	t_pop();

	cache->hdr_modified = TRUE;
	return 0;
}
