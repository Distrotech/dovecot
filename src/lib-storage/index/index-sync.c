/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "seq-range-array.h"
#include "array.h"
#include "buffer.h"
#include "index-storage.h"

struct index_mailbox_sync_context {
	struct mailbox_sync_context ctx;
	struct index_mailbox *ibox;
	struct mail_index_view_sync_ctx *sync_ctx;
	uint32_t messages_count;

	const ARRAY_TYPE(seq_range) *expunges;
	unsigned int expunge_pos;
	uint32_t last_seq1, last_seq2;

	bool failed;
};

void index_mailbox_set_recent_uid(struct index_mailbox *ibox, uint32_t uid)
{
	if (uid <= ibox->recent_flags_prev_uid) {
		i_assert(seq_range_exists(&ibox->recent_flags, uid));
		return;
	}
	ibox->recent_flags_prev_uid = uid;

	seq_range_array_add(&ibox->recent_flags, 64, uid);
	ibox->recent_flags_count++;
}

void index_mailbox_set_recent_seq(struct index_mailbox *ibox,
				  struct mail_index_view *view,
				  uint32_t seq1, uint32_t seq2)
{
	uint32_t uid;
	int ret;

	for (; seq1 <= seq2; seq1++) {
		ret = mail_index_lookup_uid(view, seq1, &uid);
		i_assert(ret == 0);
		index_mailbox_set_recent_uid(ibox, uid);
	}
}

bool index_mailbox_is_recent(struct index_mailbox *ibox, uint32_t uid)
{
	return array_is_created(&ibox->recent_flags) &&
		seq_range_exists(&ibox->recent_flags, uid);
}

unsigned int index_mailbox_get_recent_count(struct index_mailbox *ibox)
{
	const struct mail_index_header *hdr;
	const struct seq_range *range;
	unsigned int i, count, recent_count;

	if (!array_is_created(&ibox->recent_flags))
		return 0;

	hdr = mail_index_get_header(ibox->view);
	recent_count = ibox->recent_flags_count;
	range = array_get(&ibox->recent_flags, &count);
	for (i = count; i > 0; ) {
		i--;
		if (range[i].seq2 < hdr->next_uid)
			break;

		if (range[i].seq1 >= hdr->next_uid) {
			/* completely invisible to this view */
			recent_count -= range[i].seq2 - range[i].seq1 + 1;
		} else {
			/* partially invisible */
			recent_count -= range[i].seq2 - hdr->next_uid + 1;
			break;
		}
	}
	return recent_count;
}

static void index_mailbox_expunge_recent(struct index_mailbox *ibox,
					 uint32_t seq1, uint32_t seq2)
{
	uint32_t uid;
	int ret;

	if (!array_is_created(&ibox->recent_flags))
		return;

	for (; seq1 <= seq2; seq1++) {
		ret = mail_index_lookup_uid(ibox->view, seq1, &uid);
		i_assert(ret == 0);

		if (seq_range_array_remove(&ibox->recent_flags, uid))
			ibox->recent_flags_count--;
	}
}

struct mailbox_sync_context *
index_mailbox_sync_init(struct mailbox *box, enum mailbox_sync_flags flags,
			bool failed)
{
	struct index_mailbox *ibox = (struct index_mailbox *)box;
        struct index_mailbox_sync_context *ctx;
	enum mail_index_view_sync_flags sync_flags = 0;

	ctx = i_new(struct index_mailbox_sync_context, 1);
	ctx->ctx.box = box;
	ctx->ibox = ibox;

	if (failed) {
		ctx->failed = TRUE;
		return &ctx->ctx;
	}

	ctx->messages_count = mail_index_view_get_messages_count(ibox->view);

	if ((flags & MAILBOX_SYNC_FLAG_NO_EXPUNGES) != 0)
		sync_flags = MAIL_INDEX_VIEW_SYNC_FLAG_NOEXPUNGES;

	if (mail_index_view_sync_begin(ibox->view, sync_flags,
				       &ctx->sync_ctx) < 0) {
		mail_storage_set_index_error(ibox);
		ctx->failed = TRUE;
		return &ctx->ctx;
	}

	if ((flags & MAILBOX_SYNC_FLAG_NO_EXPUNGES) == 0) {
		mail_index_view_sync_get_expunges(ctx->sync_ctx,
						  &ctx->expunges);
		ctx->expunge_pos = array_count(ctx->expunges);
	}
	return &ctx->ctx;
}

static bool sync_rec_check_skips(struct index_mailbox_sync_context *ctx,
				 struct mailbox_sync_rec *sync_rec)
{
	uint32_t seq, new_seq1, new_seq2;

	if (sync_rec->seq1 >= ctx->last_seq1 &&
	    sync_rec->seq1 <= ctx->last_seq2)
		new_seq1 = ctx->last_seq2 + 1;
	else
		new_seq1 = sync_rec->seq1;
	if (sync_rec->seq2 >= ctx->last_seq1 &&
	    sync_rec->seq2 <= ctx->last_seq2)
		new_seq2 = ctx->last_seq1 - 1;
	else
		new_seq2 = sync_rec->seq2;

	if (new_seq1 > new_seq2)
		return FALSE;

	ctx->last_seq1 = sync_rec->seq1;
	ctx->last_seq2 = sync_rec->seq2;

	sync_rec->seq1 = new_seq1;
	sync_rec->seq2 = new_seq2;

	/* FIXME: we're only skipping messages from the beginning and from
	   the end. we should skip also the middle ones. This takes care of
	   the most common repeats though. */
	if (ctx->expunges != NULL) {
		/* skip expunged messages from the beginning and the end */
		for (seq = sync_rec->seq1; seq <= sync_rec->seq2; seq++) {
			if (!seq_range_exists(ctx->expunges, seq))
				break;
		}
		if (seq > sync_rec->seq2) {
			/* everything skipped */
			return FALSE;
		}
		sync_rec->seq1 = seq;

		for (seq = sync_rec->seq2; seq >= sync_rec->seq1; seq--) {
			if (!seq_range_exists(ctx->expunges, seq))
				break;
		}
		sync_rec->seq2 = seq;
	}
	return TRUE;
}

int index_mailbox_sync_next(struct mailbox_sync_context *_ctx,
			    struct mailbox_sync_rec *sync_rec_r)
{
	struct index_mailbox_sync_context *ctx =
		(struct index_mailbox_sync_context *)_ctx;
	struct mail_index_view_sync_rec sync;
	int ret;

	if (ctx->failed)
		return -1;

	while ((ret = mail_index_view_sync_next(ctx->sync_ctx, &sync)) > 0) {
		switch (sync.type) {
		case MAIL_INDEX_SYNC_TYPE_APPEND:
			/* not interested */
			break;
		case MAIL_INDEX_SYNC_TYPE_EXPUNGE:
			/* later */
			break;
		case MAIL_INDEX_SYNC_TYPE_FLAGS:
		case MAIL_INDEX_SYNC_TYPE_KEYWORD_ADD:
		case MAIL_INDEX_SYNC_TYPE_KEYWORD_REMOVE:
		case MAIL_INDEX_SYNC_TYPE_KEYWORD_RESET:
			/* FIXME: hide the flag updates for expunged messages */
			if (mail_index_lookup_uid_range(ctx->ibox->view,
						sync.uid1, sync.uid2,
						&sync_rec_r->seq1,
						&sync_rec_r->seq2) < 0) {
				ctx->failed = TRUE;
				return -1;
			}

			if (sync_rec_r->seq1 == 0)
				break;

			if (!sync_rec_check_skips(ctx, sync_rec_r))
				break;

			sync_rec_r->type =
				sync.type == MAIL_INDEX_SYNC_TYPE_FLAGS ?
				MAILBOX_SYNC_TYPE_FLAGS :
				MAILBOX_SYNC_TYPE_KEYWORDS;
			return 1;
		}
	}
	if (ret < 0) {
		mail_storage_set_index_error(ctx->ibox);
		return -1;
	}

	if (ctx->expunge_pos > 0) {
		/* expunges is a sorted array of sequences. it's easiest for
		   us to print them from end to beginning. */
		const struct seq_range *range;

		ctx->expunge_pos--;
		range = array_idx(ctx->expunges, ctx->expunge_pos);

		sync_rec_r->seq1 = range->seq1;
		sync_rec_r->seq2 = range->seq2;
		index_mailbox_expunge_recent(ctx->ibox, sync_rec_r->seq1,
					     sync_rec_r->seq2);

		if (sync_rec_r->seq2 > ctx->messages_count)
			sync_rec_r->seq2 = ctx->messages_count;
		ctx->messages_count -= sync_rec_r->seq2 - sync_rec_r->seq1 + 1;

		sync_rec_r->type = MAILBOX_SYNC_TYPE_EXPUNGE;
		return 1;
	}

	return 0;
}

int index_mailbox_sync_deinit(struct mailbox_sync_context *_ctx,
			      enum mailbox_status_items status_items,
			      struct mailbox_status *status_r)
{
	struct index_mailbox_sync_context *ctx =
		(struct index_mailbox_sync_context *)_ctx;
	struct index_mailbox *ibox = ctx->ibox;
	const struct mail_index_header *hdr;
	uint32_t seq1, seq2;
	int ret = ctx->failed ? -1 : 0;

	if (ctx->sync_ctx != NULL)
		mail_index_view_sync_end(&ctx->sync_ctx);

	if (ibox->keep_recent) {
		/* mailbox syncing didn't necessarily update our recent state */
		hdr = mail_index_get_header(ibox->view);
		if (hdr->first_recent_uid > ibox->recent_flags_prev_uid) {
			if (mail_index_lookup_uid_range(ibox->view,
							hdr->first_recent_uid,
							hdr->next_uid,
							&seq1, &seq2) < 0) {
				mail_storage_set_index_error(ctx->ibox);
				return -1;
			}
			if (seq1 != 0) {
				index_mailbox_set_recent_seq(ibox, ibox->view,
							     seq1, seq2);
			}
		}
	}

	if (ret == 0) {
		ret = status_items == 0 ? 0 :
			index_storage_get_status_locked(ctx->ibox, status_items,
							status_r);
	}

	mail_index_view_unlock(ibox->view);
	i_free(ctx);
	return ret;
}

bool index_keyword_array_cmp(const ARRAY_TYPE(keyword_indexes) *k1,
			     const ARRAY_TYPE(keyword_indexes) *k2)
{
	const unsigned int *idx1, *idx2;
	unsigned int i, j, count1, count2;

	if (!array_is_created(k1))
		return !array_is_created(k2) || array_count(k2) == 0;
	if (!array_is_created(k2))
		return array_count(k1) == 0;

	/* The arrays may not be sorted, but they usually are. Optimize for
	   the assumption that they are */
	idx1 = array_get(k1, &count1);
	idx2 = array_get(k2, &count2);

	if (count1 != count2)
		return FALSE;

	for (i = 0; i < count1; i++) {
		if (idx1[i] != idx2[i]) {
			/* not found / unsorted array. check. */
			for (j = 0; j < count1; j++) {
				if (idx1[i] == idx2[j])
					break;
			}
			if (j == count1)
				return FALSE;
		}
	}
	return TRUE;
}
