/* Copyright (C) 2003 Timo Sirainen */

#include "lib.h"
#include "istream.h"
#include "index-mail.h"
#include "maildir-storage.h"
#include "maildir-filename.h"
#include "maildir-uidlist.h"

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int
do_open(struct maildir_mailbox *mbox, const char *path, int *fd)
{
	*fd = open(path, O_RDONLY);
	if (*fd != -1)
		return 1;
	if (errno == ENOENT)
		return 0;

	mail_storage_set_critical(&mbox->storage->storage,
				  "open(%s) failed: %m", path);
	return -1;
}

static int
do_stat(struct maildir_mailbox *mbox, const char *path, struct stat *st)
{
	if (stat(path, st) == 0)
		return 1;
	if (errno == ENOENT)
		return 0;

	mail_storage_set_critical(&mbox->storage->storage,
				  "stat(%s) failed: %m", path);
	return -1;
}

static struct istream *
maildir_open_mail(struct maildir_mailbox *mbox, struct mail *mail,
		  bool *deleted_r)
{
	const char *path;
	int fd = -1;

	*deleted_r = FALSE;

	if (mail->uid != 0) {
		if (maildir_file_do(mbox, mail->uid, do_open, &fd) < 0)
			return NULL;
	} else {
		path = maildir_save_file_get_path(mail->transaction, mail->seq);
		if (do_open(mbox, path, &fd) <= 0)
			return NULL;
	}

	if (fd == -1) {
		*deleted_r = TRUE;
		return NULL;
	}

	return i_stream_create_fd(fd, MAIL_READ_BLOCK_SIZE, TRUE);
}

static int maildir_mail_stat(struct mail *mail, struct stat *st)
{
	struct maildir_mailbox *mbox = (struct maildir_mailbox *)mail->box;
	struct index_mail_data *data = &((struct index_mail *)mail)->data;
	const char *path;
	int fd, ret;

	if (data->access_part != 0 && data->stream == NULL) {
		/* we're going to open the mail anyway */
		(void)mail_get_stream(mail, NULL, NULL);
	}

	if (data->stream != NULL) {
		fd = i_stream_get_fd(data->stream);
		i_assert(fd != -1);

		if (fstat(fd, st) < 0) {
			mail_storage_set_critical(&mbox->storage->storage,
						  "fstat(maildir) failed: %m");
			return -1;
		}
	} else if (mail->uid != 0) {
		ret = maildir_file_do(mbox, mail->uid, do_stat, st);
		if (ret <= 0) {
			if (ret == 0)
				mail_set_expunged(mail);
			return -1;
		}
	} else {
		path = maildir_save_file_get_path(mail->transaction, mail->seq);
		if (do_stat(mbox, path, st) <= 0)
			return -1;
	}
	return 0;
}

static time_t maildir_mail_get_received_date(struct mail *_mail)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	struct stat st;
	uint32_t t;

	(void)index_mail_get_received_date(_mail);
	if (data->received_date != (time_t)-1)
		return data->received_date;

	if (maildir_mail_stat(_mail, &st) < 0)
		return (time_t)-1;

	data->received_date = t = st.st_mtime;
	index_mail_cache_add(mail, MAIL_CACHE_RECEIVED_DATE, &t, sizeof(t));
	return data->received_date;
}

static time_t maildir_mail_get_save_date(struct mail *_mail)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct index_mail_data *data = &mail->data;
	struct stat st;
	uint32_t t;

	(void)index_mail_get_save_date(_mail);
	if (data->save_date != (time_t)-1)
		return data->save_date;

	if (maildir_mail_stat(_mail, &st) < 0)
		return (time_t)-1;

	data->save_date = t = st.st_ctime;
	index_mail_cache_add(mail, MAIL_CACHE_SAVE_DATE, &t, sizeof(t));
	return data->save_date;
}

static bool
maildir_mail_get_fname(struct maildir_mailbox *mbox, struct mail *mail,
		       const char **fname_r)
{
	enum maildir_uidlist_rec_flag flags;

	*fname_r = maildir_uidlist_lookup(mbox->uidlist, mail->uid, &flags);
	if (*fname_r == NULL) {
		mail_set_expunged(mail);
		return FALSE;
	}
	return TRUE;
}

static int maildir_get_pop3_state(struct index_mail *mail)
{
	const struct mail_cache_field *fields;
	unsigned int i, count, vsize_idx;
	enum mail_cache_decision_type dec, vsize_dec;
	enum mail_fetch_field allowed_pop3_fields;
	bool not_pop3_only = FALSE;

	if (mail->pop3_state_set)
		return mail->pop3_state;

	/* if this mail itself has non-pop3 fields we know we're not
	   pop3-only */
	allowed_pop3_fields = MAIL_FETCH_FLAGS | MAIL_FETCH_STREAM_HEADER |
		MAIL_FETCH_STREAM_BODY | MAIL_FETCH_UIDL_FILE_NAME |
		MAIL_FETCH_VIRTUAL_SIZE;

	if (mail->wanted_headers != NULL ||
	    (mail->wanted_fields & allowed_pop3_fields) == 0)
		not_pop3_only = TRUE;

	/* get vsize decision */
	vsize_idx = mail->ibox->cache_fields[MAIL_CACHE_VIRTUAL_FULL_SIZE].idx;
	if (not_pop3_only) {
		vsize_dec = mail_cache_field_get_decision(mail->ibox->cache,
							  vsize_idx);
		vsize_dec &= ~MAIL_CACHE_DECISION_FORCED;
	} else {
		/* also check if there are any non-vsize cached fields */
		vsize_dec = MAIL_CACHE_DECISION_NO;
		fields = mail_cache_register_get_list(mail->ibox->cache,
						      pool_datastack_create(),
						      &count);
		for (i = 0; i < count; i++) {
			dec = fields[i].decision & ~MAIL_CACHE_DECISION_FORCED;
			if (fields[i].idx == vsize_idx)
				vsize_dec = dec;
			else if (dec != MAIL_CACHE_DECISION_NO)
				not_pop3_only = TRUE;
		}
	}

	if (!not_pop3_only) {
		/* either nothing is cached, or only vsize is cached. */
		mail->pop3_state = 1;
	} else if (vsize_dec != MAIL_CACHE_DECISION_YES) {
		/* if virtual size isn't cached permanently,
		   POP3 isn't being used */
		mail->pop3_state = -1;
	} else {
		/* possibly a mixed pop3/imap */
		mail->pop3_state = 0;
	}
	mail->pop3_state_set = TRUE;
	return mail->pop3_state;
}

static uoff_t maildir_mail_get_virtual_size(struct mail *_mail)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct maildir_mailbox *mbox = (struct maildir_mailbox *)mail->ibox;
	struct index_mail_data *data = &mail->data;
	struct message_size hdr_size, body_size;
	const char *path, *fname, *value;
	uoff_t old_offset;
	int pop3_state;

	if (index_mail_get_cached_virtual_size(mail) != (uoff_t)-1)
		return data->virtual_size;

	if (_mail->uid != 0) {
		if (!maildir_mail_get_fname(mbox, _mail, &fname))
			return (uoff_t)-1;
	} else {
		path = maildir_save_file_get_path(_mail->transaction,
						  _mail->seq);
		fname = strrchr(path, '/');
		fname = fname != NULL ? fname + 1 : path;
	}

	/* size can be included in filename */
	if (maildir_filename_get_size(fname, MAILDIR_EXTRA_VIRTUAL_SIZE,
				      &data->virtual_size))
		return data->virtual_size;

	/* size can be included in uidlist entry */
	if (_mail->uid != 0) {
		value = maildir_uidlist_lookup_ext(mbox->uidlist, _mail->uid,
						MAILDIR_UIDLIST_REC_EXT_VSIZE);
		if (value != NULL) {
			char *p;

			data->virtual_size = strtoull(value, &p, 10);
			if (*p == '\0')
				return data->virtual_size;
		}
	}

	/* fallback to reading the file */
	old_offset = data->stream == NULL ? 0 : data->stream->v_offset;
	if (mail_get_stream(_mail, &hdr_size, &body_size) == NULL)
		return (uoff_t)-1;
	i_stream_seek(data->stream, old_offset);
	i_assert(data->virtual_size != (uoff_t)-1);

	/* 1 = pop3-only, 0 = mixed, -1 = no pop3 */
	pop3_state = maildir_get_pop3_state(mail);
	if (pop3_state <= 0) {
		index_mail_cache_add(mail, MAIL_CACHE_VIRTUAL_FULL_SIZE,
				     &data->virtual_size,
				     sizeof(data->virtual_size));
	}
	if (pop3_state >= 0) {
		/* if virtual size is wanted permanently, store it to uidlist
		   so that in case cache file gets lost we can get it quickly */
		maildir_uidlist_set_ext(mbox->uidlist, _mail->uid,
					MAILDIR_UIDLIST_REC_EXT_VSIZE,
					dec2str(data->virtual_size));
	}
	return data->virtual_size;
}

static const char *
maildir_mail_get_special(struct mail *_mail, enum mail_fetch_field field)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct maildir_mailbox *mbox = (struct maildir_mailbox *)mail->ibox;
	const char *path, *fname, *end;

	if (field == MAIL_FETCH_UIDL_FILE_NAME) {
		if (_mail->uid != 0) {
			if (!maildir_mail_get_fname(mbox, _mail, &fname))
				return NULL;
		} else {
			path = maildir_save_file_get_path(_mail->transaction,
							  _mail->seq);
			fname = strrchr(path, '/');
			fname = fname != NULL ? fname + 1 : path;
		}
		end = strchr(fname, MAILDIR_INFO_SEP);
		return end == NULL ? fname : t_strdup_until(fname, end);
	}

	return index_mail_get_special(_mail, field);
}
							
static uoff_t maildir_mail_get_physical_size(struct mail *_mail)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct maildir_mailbox *mbox = (struct maildir_mailbox *)mail->ibox;
	struct index_mail_data *data = &mail->data;
	struct stat st;
	const char *path, *fname;
	uoff_t size;
	int ret;

	size = index_mail_get_physical_size(_mail);
	if (size != (uoff_t)-1)
		return size;

	if (_mail->uid != 0) {
		if (!maildir_mail_get_fname(mbox, _mail, &fname))
			return (uoff_t)-1;
		path = NULL;
	} else {
		path = maildir_save_file_get_path(_mail->transaction,
						  _mail->seq);
		fname = strrchr(path, '/');
		fname = fname != NULL ? fname + 1 : path;
	}

	/* size can be included in filename */
	if (!maildir_filename_get_size(fname, MAILDIR_EXTRA_FILE_SIZE, &size)) {
		if (_mail->uid != 0) {
			ret = maildir_file_do(mbox, _mail->uid, do_stat, &st);
			if (ret <= 0) {
				if (ret == 0)
					mail_set_expunged(_mail);
				return (uoff_t)-1;
			}
		} else {
			/* saved mail which hasn't been committed yet */
			if (do_stat(mbox, path, &st) <= 0)
				return (uoff_t)-1;
		}
		size = st.st_size;
	}

	index_mail_cache_add(mail, MAIL_CACHE_PHYSICAL_FULL_SIZE,
			     &size, sizeof(size));
	data->physical_size = size;
	return size;
}

static struct istream *maildir_mail_get_stream(struct mail *_mail,
					       struct message_size *hdr_size,
					       struct message_size *body_size)
{
	struct index_mail *mail = (struct index_mail *)_mail;
	struct maildir_mailbox *mbox = (struct maildir_mailbox *)mail->ibox;
	struct index_mail_data *data = &mail->data;
	bool deleted;

	if (data->stream == NULL) {
		data->stream = maildir_open_mail(mbox, _mail, &deleted);
		if (data->stream == NULL) {
			if (deleted)
				mail_set_expunged(_mail);
			return NULL;
		}
	}

	return index_mail_init_stream(mail, hdr_size, body_size);
}

struct mail_vfuncs maildir_mail_vfuncs = {
	index_mail_free,
	index_mail_set_seq,
	index_mail_set_uid,

	index_mail_get_flags,
	index_mail_get_keywords,
	index_mail_get_parts,
	index_mail_get_date,
	maildir_mail_get_received_date,
	maildir_mail_get_save_date,
	maildir_mail_get_virtual_size,
	maildir_mail_get_physical_size,
	index_mail_get_first_header,
	index_mail_get_headers,
	index_mail_get_header_stream,
	maildir_mail_get_stream,
	maildir_mail_get_special,
	index_mail_update_flags,
	index_mail_update_keywords,
	index_mail_expunge
};
