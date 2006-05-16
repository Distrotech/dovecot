/* Copyright (C) 2003 Timo Sirainen */

#include "lib.h"
#include "istream.h"
#include "index-mail.h"
#include "mbox-storage.h"
#include "mbox-file.h"
#include "mbox-lock.h"
#include "mbox-sync-private.h"
#include "istream-raw-mbox.h"
#include "istream-header-filter.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static void mbox_prepare_resync(struct index_mail *mail)
{
	struct mbox_transaction_context *t =
		(struct mbox_transaction_context *)mail->trans;
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)mail->ibox;

	if (mbox->mbox_lock_type == F_RDLCK) {
		if (mbox->mbox_lock_id == t->mbox_lock_id)
			t->mbox_lock_id = 0;
		(void)mbox_unlock(mbox, mbox->mbox_lock_id);
		mbox->mbox_lock_id = 0;
		i_assert(mbox->mbox_lock_type == F_UNLCK);
	}
}

static int mbox_mail_seek(struct index_mail *mail)
{
	struct mbox_transaction_context *t =
		(struct mbox_transaction_context *)mail->trans;
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)mail->ibox;
	enum mbox_sync_flags sync_flags = 0;
	int ret;
	bool deleted;

	if (mail->mail.mail.expunged)
		return 0;

__again:
	if (mbox->mbox_lock_type == F_UNLCK) {
		sync_flags |= MBOX_SYNC_LOCK_READING;
		if (mbox_sync(mbox, sync_flags) < 0)
			return -1;

		/* refresh index file after mbox has been locked to make
		   sure we get only up-to-date mbox offsets. */
		if (mail_index_refresh(mbox->ibox.index) < 0) {
			mail_storage_set_index_error(&mbox->ibox);
			return -1;
		}

		i_assert(mbox->mbox_lock_type != F_UNLCK);
		t->mbox_lock_id = mbox->mbox_lock_id;
	} else if ((sync_flags & MBOX_SYNC_FORCE_SYNC) != 0) {
		/* dirty offsets are broken and mbox is write-locked.
		   sync it to update offsets. */
		if (mbox_sync(mbox, sync_flags) < 0)
			return -1;
	}

	if (mbox_file_open_stream(mbox) < 0)
		return -1;

	ret = mbox_file_seek(mbox, mail->trans->trans_view,
			     mail->mail.mail.seq, &deleted);
	if (ret < 0) {
		if (deleted) {
			mail->mail.mail.expunged = TRUE;
			return 0;
		}
		return -1;
	}

	if (ret == 0) {
		/* we'll need to re-sync it completely */
                mbox_prepare_resync(mail);
		sync_flags |= MBOX_SYNC_UNDIRTY | MBOX_SYNC_FORCE_SYNC;
		goto __again;
	}

	return 1;
}

static time_t mbox_mail_get_received_date(struct mail *_mail)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)mail->ibox;

	(void)index_mail_get_received_date(_mail);
	if (data->received_date != (time_t)-1)
		return data->received_date;

	if (mbox_mail_seek(mail) <= 0)
		return (time_t)-1;
	data->received_date =
		istream_raw_mbox_get_received_time(mbox->mbox_stream);
	if (data->received_date == (time_t)-1) {
		/* it's broken and conflicts with our "not found"
		   return value. change it. */
		data->received_date = 0;
	}

	index_mail_cache_add(mail, MAIL_CACHE_RECEIVED_DATE,
			     &data->received_date, sizeof(data->received_date));
	return data->received_date;
}

static const char *
mbox_mail_get_special(struct mail *_mail, enum mail_fetch_field field)
{
#define EMPTY_MD5_SUM "00000000000000000000000000000000"
	struct index_mail *mail = (struct index_mail *)_mail;
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)mail->ibox;
	const char *value;

	switch (field) {
	case MAIL_FETCH_FROM_ENVELOPE:
		if (mbox_mail_seek(mail) <= 0)
			return NULL;

		return istream_raw_mbox_get_sender(mbox->mbox_stream);
	case MAIL_FETCH_HEADER_MD5:
		value = index_mail_get_special(_mail, field);
		if (value != NULL && strcmp(value, EMPTY_MD5_SUM) != 0)
			return value;

		/* i guess in theory the EMPTY_MD5_SUM is valid and can happen,
		   but it's almost guaranteed that it means the MD5 sum is
		   missing. recalculate it. */
		mbox->mbox_save_md5 = TRUE;
                mbox_prepare_resync(mail);
		if (mbox_sync(mbox, MBOX_SYNC_FORCE_SYNC) < 0)
			return NULL;
		break;
	default:
		break;
	}

	return index_mail_get_special(_mail, field);
}

static uoff_t mbox_mail_get_physical_size(struct mail *_mail)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)mail->ibox;
	struct istream *stream;
	uoff_t hdr_offset, body_offset, body_size;

	if (mbox_mail_seek(mail) <= 0)
		return (uoff_t)-1;

	/* our header size varies, so don't do any caching */
	stream = mbox->mbox_stream;
	hdr_offset = istream_raw_mbox_get_header_offset(stream);
	body_offset = istream_raw_mbox_get_body_offset(stream);
	if (body_offset == (uoff_t)-1)
		return (uoff_t)-1;
	body_size = istream_raw_mbox_get_body_size(stream, (uoff_t)-1);

	data->physical_size = (body_offset - hdr_offset) + body_size;
	return data->physical_size;

}

static struct istream *mbox_mail_get_stream(struct mail *_mail,
					    struct message_size *hdr_size,
					    struct message_size *body_size)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)mail->ibox;
	struct istream *raw_stream;
	uoff_t offset;

	if (data->stream == NULL) {
		if (mbox_mail_seek(mail) <= 0)
			return NULL;

		raw_stream = mbox->mbox_stream;
		offset = istream_raw_mbox_get_header_offset(raw_stream);
		raw_stream = i_stream_create_limit(default_pool, raw_stream,
						   offset, (uoff_t)-1);
		data->stream =
			i_stream_create_header_filter(raw_stream,
						      HEADER_FILTER_EXCLUDE,
						      mbox_hide_headers,
						      mbox_hide_headers_count,
						      NULL, NULL);
		i_stream_unref(&raw_stream);
	}

	return index_mail_init_stream(mail, hdr_size, body_size);
}

struct mail_vfuncs mbox_mail_vfuncs = {
	index_mail_free,
	index_mail_set_seq,
	index_mail_set_uid,

	index_mail_get_flags,
	index_mail_get_keywords,
	index_mail_get_parts,
	mbox_mail_get_received_date,
	index_mail_get_date,
	index_mail_get_virtual_size,
	mbox_mail_get_physical_size,
	index_mail_get_first_header,
	index_mail_get_headers,
	index_mail_get_header_stream,
	mbox_mail_get_stream,
	mbox_mail_get_special,
	index_mail_update_flags,
	index_mail_update_keywords,
	index_mail_expunge
};
