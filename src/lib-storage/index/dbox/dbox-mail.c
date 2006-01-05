/* Copyright (C) 2005 Timo Sirainen */

#include "lib.h"
#include "hex-dec.h"
#include "read-full.h"
#include "istream.h"
#include "index-mail.h"
#include "dbox-file.h"
#include "dbox-sync.h"
#include "dbox-storage.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int dbox_mail_parse_mail_header(struct index_mail *mail,
				       struct dbox_file *file)
{
	struct dbox_mailbox *mbox =
		(struct dbox_mailbox *)mail->mail.mail.box;
	const struct dbox_mail_header *hdr = &file->seeked_mail_header;
	uint32_t hdr_uid = hex2dec(hdr->uid_hex, sizeof(hdr->uid_hex));

	if (hdr_uid != mail->mail.mail.uid ||
	    memcmp(hdr->magic, DBOX_MAIL_HEADER_MAGIC,
		   sizeof(hdr->magic)) != 0) {
		mail_storage_set_critical(STORAGE(mbox->storage),
			"dbox %s: Cached file offset broken",
			mbox->file->path);

		/* make sure we get it fixed */
		(void)dbox_sync(mbox, TRUE);
		return -1;
	}

	if (hdr->expunged == '1') {
		mail->mail.mail.expunged = TRUE;
		return 0;
	}

	mail->data.physical_size =
		hex2dec(hdr->mail_size_hex, sizeof(hdr->mail_size_hex));
	mail->data.received_date =
		hex2dec(hdr->received_time_hex, sizeof(hdr->received_time_hex));
	return 1;
}

int dbox_mail_lookup_offset(struct index_transaction_context *trans,
			    uint32_t seq, uint32_t *file_seq_r,
			    uoff_t *offset_r)
{
	struct dbox_mailbox *mbox =
		(struct dbox_mailbox *)trans->ibox;
	int synced = FALSE, ret;

	for (;;) {
		ret = dbox_file_lookup_offset(mbox, trans->trans_view, seq,
					      file_seq_r, offset_r);
		if (ret <= 0)
			return ret;
		if (*file_seq_r != 0)
			return 1;

		/* lost file sequence/offset */
		if (synced)
			return -1;

		mail_storage_set_critical(STORAGE(mbox->storage),
			"Cached message offset lost for seq %u in "
			"dbox file %s", seq, mbox->path);

		/* resync and try again */
		if (dbox_sync(mbox, TRUE) < 0)
			return -1;
		synced = TRUE;
	}
}

static int dbox_mail_open(struct index_mail *mail, uoff_t *offset_r)
{
	struct dbox_mailbox *mbox = (struct dbox_mailbox *)mail->ibox;
	uint32_t seq = mail->mail.mail.seq;
	uint32_t file_seq, prev_file_seq = 0;
	uoff_t offset, prev_offset = 0;
	int i, ret;

	if (mail->mail.mail.expunged)
		return 0;

	for (i = 0; i < 3; i++) {
		ret = dbox_mail_lookup_offset(mail->trans, seq,
					      &file_seq, &offset);
		if (ret <= 0) {
			if (ret == 0)
				mail->mail.mail.expunged = TRUE;
			return ret;
		}

		if ((ret = dbox_file_seek(mbox, file_seq, offset)) < 0)
			return -1;
		if (ret > 0) {
			/* ok */
			*offset_r = offset;
			return dbox_mail_parse_mail_header(mail, mbox->file);
		}

		if (prev_file_seq == file_seq && prev_offset == offset) {
			/* broken offset */
			break;
		}

		/* mail was moved. resync index file to find out the new offset
		   and try again. */
		if (mail_index_refresh(mbox->ibox.index) < 0) {
			mail_storage_set_index_error(&mbox->ibox);
			return -1;
		}
		prev_file_seq = file_seq;
		prev_offset = offset;
	}

	mail_storage_set_critical(STORAGE(mbox->storage),
				  "Cached message offset lost for seq %u in "
				  "dbox file %s", seq, mbox->path);
	return -1;
}

static time_t dbox_mail_get_received_date(struct mail *_mail)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	uoff_t offset;

	(void)index_mail_get_received_date(_mail);
	if (data->received_date != (time_t)-1)
		return data->received_date;

	if (dbox_mail_open(mail, &offset) <= 0)
		return (time_t)-1;
	if (data->received_date == (time_t)-1) {
		/* it's broken and conflicts with our "not found"
		   return value. change it. */
		data->received_date = 0;
	}

	mail_cache_add(mail->trans->cache_trans, mail->data.seq,
		       MAIL_CACHE_RECEIVED_DATE,
		       &data->received_date, sizeof(data->received_date));
	return data->received_date;
}

static uoff_t dbox_mail_get_physical_size(struct mail *_mail)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	uoff_t offset;

	(void)index_mail_get_physical_size(_mail);
	if (data->physical_size != (uoff_t)-1)
		return data->physical_size;

	if (dbox_mail_open(mail, &offset) <= 0)
		return (uoff_t)-1;

	mail_cache_add(mail->trans->cache_trans, mail->data.seq,
		       MAIL_CACHE_PHYSICAL_FULL_SIZE,
		       &data->physical_size, sizeof(data->physical_size));
	return data->physical_size;

}

static struct istream *
dbox_mail_get_stream(struct mail *_mail,
		     struct message_size *hdr_size,
		     struct message_size *body_size)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct dbox_mailbox *mbox = (struct dbox_mailbox *)mail->ibox;
	uoff_t offset;

	if (mail->data.stream == NULL) {
		if (dbox_mail_open(mail, &offset) <= 0)
			return NULL;

		offset += mbox->file->mail_header_size;
		mail->data.stream =
			i_stream_create_limit(default_pool, mbox->file->input,
					      offset,
					      mbox->file->seeked_mail_size);
	}

	return index_mail_init_stream(mail, hdr_size, body_size);
}

struct mail_vfuncs dbox_mail_vfuncs = {
	index_mail_free,
	index_mail_set_seq,

	index_mail_get_flags,
	index_mail_get_keywords,
	index_mail_get_parts,
	dbox_mail_get_received_date,
	index_mail_get_date,
	index_mail_get_virtual_size,
	dbox_mail_get_physical_size,
	index_mail_get_first_header,
	index_mail_get_headers,
	index_mail_get_header_stream,
	dbox_mail_get_stream,
	index_mail_get_special,
	index_mail_update_flags,
	index_mail_update_keywords,
	index_mail_expunge
};
