/* Copyright (C) 2003 Timo Sirainen */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "hash.h"
#include "istream.h"
#include "ostream.h"
#include "str.h"
#include "file-dotlock.h"
#include "close-keep-errno.h"
#include "nfs-workarounds.h"
#include "write-full.h"
#include "maildir-storage.h"
#include "maildir-sync.h"
#include "maildir-filename.h"
#include "maildir-uidlist.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

/* NFS: How many times to retry reading dovecot-uidlist file if ESTALE
   error occurs in the middle of reading it */
#define UIDLIST_ESTALE_RETRY_COUNT NFS_ESTALE_RETRY_COUNT

/* how many seconds to wait before overriding uidlist.lock */
#define UIDLIST_LOCK_STALE_TIMEOUT (60*2)

#define UIDLIST_COMPRESS_PERCENTAGE 75

#define UIDLIST_IS_LOCKED(uidlist) \
	((uidlist)->lock_count > 0)

struct maildir_uidlist_rec {
	uint32_t uid;
	uint32_t flags;
	char *filename;
};
ARRAY_DEFINE_TYPE(maildir_uidlist_rec_p, struct maildir_uidlist_rec *);

struct maildir_uidlist {
	struct maildir_mailbox *mbox;
	char *path;

	int fd;
	dev_t fd_dev;
	ino_t fd_ino;
	off_t fd_size;

	unsigned int lock_count;

	struct dotlock_settings dotlock_settings;
	struct dotlock *dotlock;

	pool_t record_pool;
	ARRAY_TYPE(maildir_uidlist_rec_p) records;
	struct hash_table *files;
	unsigned int change_counter;

	unsigned int version;
	unsigned int uid_validity, next_uid, prev_read_uid, last_seen_uid;
	unsigned int read_records_count;
	uint32_t first_recent_uid;
	uoff_t last_read_offset;

	unsigned int recreate:1;
	unsigned int initial_read:1;
	unsigned int initial_sync:1;
};

struct maildir_uidlist_sync_ctx {
	struct maildir_uidlist *uidlist;
	enum maildir_uidlist_sync_flags sync_flags;

	pool_t record_pool;
	ARRAY_TYPE(maildir_uidlist_rec_p) records;
	struct hash_table *files;

	unsigned int first_new_pos;
	unsigned int new_files_count;

	unsigned int partial:1;
	unsigned int finished:1;
	unsigned int changed:1;
	unsigned int failed:1;
};

struct maildir_uidlist_iter_ctx {
	struct maildir_uidlist *uidlist;
	struct maildir_uidlist_rec *const *next, *const *end;

	unsigned int change_counter;
	uint32_t prev_uid;
};

static int maildir_uidlist_lock_timeout(struct maildir_uidlist *uidlist,
					bool nonblock)
{
	struct maildir_mailbox *mbox = uidlist->mbox;
	const char *path;
	mode_t old_mask;
	int ret;

	if (uidlist->lock_count > 0) {
		uidlist->lock_count++;
		return 1;
	}

	path = t_strconcat(mbox->control_dir, "/" MAILDIR_UIDLIST_NAME, NULL);
        old_mask = umask(0777 & ~mbox->mail_create_mode);
	ret = file_dotlock_create(&uidlist->dotlock_settings, path,
				  nonblock ? DOTLOCK_CREATE_FLAG_NONBLOCK : 0,
				  &uidlist->dotlock);
	umask(old_mask);
	if (ret <= 0) {
		if (ret == 0) {
			mail_storage_set_error(&mbox->storage->storage,
				MAIL_ERROR_TEMP, MAIL_ERRSTR_LOCK_TIMEOUT);
			return 0;
		}
		mail_storage_set_critical(&mbox->storage->storage,
			"file_dotlock_open(%s) failed: %m", path);
		return -1;
	}

	uidlist->lock_count++;

	/* make sure we have the latest changes before changing anything */
	if (maildir_uidlist_refresh(uidlist) < 0) {
		maildir_uidlist_unlock(uidlist);
		return -1;
	}
	return 1;
}

int maildir_uidlist_lock(struct maildir_uidlist *uidlist)
{
	return maildir_uidlist_lock_timeout(uidlist, FALSE);
}

int maildir_uidlist_try_lock(struct maildir_uidlist *uidlist)
{
	return maildir_uidlist_lock_timeout(uidlist, TRUE);
}

int maildir_uidlist_lock_touch(struct maildir_uidlist *uidlist)
{
	i_assert(UIDLIST_IS_LOCKED(uidlist));

	return file_dotlock_touch(uidlist->dotlock);
}

bool maildir_uidlist_is_locked(struct maildir_uidlist *uidlist)
{
	return UIDLIST_IS_LOCKED(uidlist);
}

void maildir_uidlist_unlock(struct maildir_uidlist *uidlist)
{
	i_assert(uidlist->lock_count > 0);

	if (--uidlist->lock_count > 0)
		return;

	(void)file_dotlock_delete(&uidlist->dotlock);
}

struct maildir_uidlist *maildir_uidlist_init(struct maildir_mailbox *mbox)
{
	struct maildir_uidlist *uidlist;

	uidlist = i_new(struct maildir_uidlist, 1);
	uidlist->fd = -1;
	uidlist->mbox = mbox;
	uidlist->path =
		i_strconcat(mbox->control_dir, "/" MAILDIR_UIDLIST_NAME, NULL);
	i_array_init(&uidlist->records, 128);
	uidlist->files = hash_create(default_pool, default_pool, 4096,
				     maildir_filename_base_hash,
				     maildir_filename_base_cmp);
	uidlist->next_uid = 1;

	uidlist->dotlock_settings.use_io_notify = TRUE;
	uidlist->dotlock_settings.use_excl_lock =
		(mbox->storage->storage.flags &
		 MAIL_STORAGE_FLAG_DOTLOCK_USE_EXCL) != 0;
	uidlist->dotlock_settings.timeout = UIDLIST_LOCK_STALE_TIMEOUT + 2;
	uidlist->dotlock_settings.stale_timeout = UIDLIST_LOCK_STALE_TIMEOUT;
	uidlist->dotlock_settings.temp_prefix = mbox->storage->temp_prefix;

	return uidlist;
}

static void maildir_uidlist_close(struct maildir_uidlist *uidlist)
{
	if (uidlist->fd != -1) {
		if (close(uidlist->fd) < 0)
			i_error("close(%s) failed: %m", uidlist->path);
		uidlist->fd = -1;
		uidlist->fd_ino = 0;
	}
	uidlist->last_read_offset = 0;
}

void maildir_uidlist_deinit(struct maildir_uidlist *uidlist)
{
	i_assert(!UIDLIST_IS_LOCKED(uidlist));

	maildir_uidlist_close(uidlist);

	hash_destroy(uidlist->files);
	if (uidlist->record_pool != NULL)
		pool_unref(uidlist->record_pool);

	array_free(&uidlist->records);
	i_free(uidlist->path);
	i_free(uidlist);
}

static void
maildir_uidlist_mark_recent(struct maildir_uidlist *uidlist, uint32_t uid)
{
	if (uidlist->first_recent_uid == 0 ||
	    uid < uidlist->first_recent_uid)
		uidlist->first_recent_uid = uid;
}

static int maildir_uidlist_next(struct maildir_uidlist *uidlist,
				const char *line)
{
        struct maildir_uidlist_rec *rec;
	uint32_t uid, flags;

	uid = flags = 0;
	while (*line >= '0' && *line <= '9') {
		uid = uid*10 + (*line - '0');
		line++;
	}

	if (uid == 0 || *line != ' ') {
		/* invalid file */
                mail_storage_set_critical(&uidlist->mbox->storage->storage,
			"Invalid data in file %s", uidlist->path);
		return 0;
	}
	if (uid <= uidlist->prev_read_uid) {
                mail_storage_set_critical(&uidlist->mbox->storage->storage,
			"UIDs not ordered in file %s (%u > %u)",
			uidlist->path, uid, uidlist->prev_read_uid);
		return 0;
	}
	uidlist->prev_read_uid = uid;

	if (uid <= uidlist->last_seen_uid) {
		/* we already have this */
		return 1;
	}
        uidlist->last_seen_uid = uid;

	while (*line == ' ') line++;

	if (uidlist->version == 2) {
		/* skip flags parameter */
		while (*line != ' ') line++;
		while (*line == ' ') line++;
	}

	if (hash_lookup_full(uidlist->files, line, NULL, NULL)) {
                mail_storage_set_critical(&uidlist->mbox->storage->storage,
			"Duplicate file in uidlist file %s: %s",
			uidlist->path, line);
		return 0;
	}

	rec = p_new(uidlist->record_pool, struct maildir_uidlist_rec, 1);
	rec->uid = uid;
	rec->flags = MAILDIR_UIDLIST_REC_FLAG_NONSYNCED;
	rec->filename = p_strdup(uidlist->record_pool, line);
	hash_insert(uidlist->files, rec->filename, rec);
	array_append(&uidlist->records, &rec, 1);
	return 1;
}

static int maildir_uidlist_read_header(struct maildir_uidlist *uidlist,
				       struct istream *input)
{
	unsigned int uid_validity, next_uid;
	const char *line;

	line = i_stream_read_next_line(input);
        if (line == NULL) {
                /* I/O error / empty file */
                return input->stream_errno == 0 ? 0 : -1;
	}

	if (sscanf(line, "%u %u %u", &uidlist->version,
		   &uid_validity, &next_uid) != 3 ||
	    uidlist->version < 1 || uidlist->version > 2) {
		/* broken file */
		mail_storage_set_critical(&uidlist->mbox->storage->storage,
			"Corrupted header in file %s (version = %u)",
			uidlist->path, uidlist->version);
		return 0;
	}
	if (uid_validity == 0 || next_uid == 0) {
		mail_storage_set_critical(&uidlist->mbox->storage->storage,
			"%s: Broken header (uidvalidity = %u, next_uid=%u)",
			uidlist->path, uid_validity, next_uid);
		return 0;
	}

	uidlist->uid_validity = uid_validity;
	uidlist->next_uid = next_uid;
	return 1;
}

static int
maildir_uidlist_update_read(struct maildir_uidlist *uidlist,
			    bool *retry_r, bool try_retry)
{
	struct mail_storage *storage = &uidlist->mbox->storage->storage;
	const char *line;
	unsigned int orig_next_uid;
	struct istream *input;
	struct stat st;
	uoff_t last_read_offset;
	int fd, ret;

	*retry_r = FALSE;

	if (uidlist->fd == -1) {
		fd = nfs_safe_open(uidlist->path, O_RDWR);
		if (fd == -1) {
			if (errno != ENOENT) {
				mail_storage_set_critical(storage,
					"open(%s) failed: %m", uidlist->path);
				return -1;
			}
			return 0;
		}
		last_read_offset = 0;
	} else {
		/* the file was updated */
		fd = uidlist->fd;
		if (lseek(fd, 0, SEEK_SET) < 0) {
			mail_storage_set_critical(storage,
				"lseek(%s) failed: %m", uidlist->path);
			return -1;
		}
		uidlist->fd = -1;
		uidlist->fd_ino = 0;
		last_read_offset = uidlist->last_read_offset;
		uidlist->last_read_offset = 0;
	}

	if (fstat(fd, &st) < 0) {
                close_keep_errno(fd);
                if (errno == ESTALE && try_retry) {
                        *retry_r = TRUE;
                        return -1;
                }
                mail_storage_set_critical(storage,
			"fstat(%s) failed: %m", uidlist->path);
		return -1;
	}

	if (uidlist->record_pool == NULL) {
		uidlist->record_pool =
			pool_alloconly_create(MEMPOOL_GROWING
					      "uidlist record_pool",
					      nearest_power(st.st_size -
							    st.st_size/8));
	}

	input = i_stream_create_file(fd, default_pool, 4096, FALSE);
	i_stream_seek(input, uidlist->last_read_offset);

	orig_next_uid = uidlist->next_uid;
	ret = input->v_offset != 0 ? 1 :
		maildir_uidlist_read_header(uidlist, input);
	if (ret > 0) {
		uidlist->prev_read_uid = 0;
		uidlist->change_counter++;
		uidlist->read_records_count = 0;

		ret = 1;
		while ((line = i_stream_read_next_line(input)) != NULL) {
			uidlist->read_records_count++;
			if (!maildir_uidlist_next(uidlist, line)) {
				ret = 0;
				break;
			}
                }
		if (input->stream_errno != 0)
                        ret = -1;

		if (uidlist->next_uid <= uidlist->prev_read_uid)
			uidlist->next_uid = uidlist->prev_read_uid + 1;
		if (uidlist->next_uid < orig_next_uid) {
			mail_storage_set_critical(storage,
				"%s: next_uid was lowered (%u -> %u)",
				uidlist->path, orig_next_uid,
				uidlist->next_uid);
			uidlist->recreate = TRUE;
			uidlist->next_uid = orig_next_uid;
		}
	}

        if (ret == 0) {
                /* file is broken */
                (void)unlink(uidlist->path);
        } else if (ret > 0) {
                /* success */
		uidlist->fd = fd;
		uidlist->fd_dev = st.st_dev;
		uidlist->fd_ino = st.st_ino;
		uidlist->fd_size = st.st_size;
		uidlist->last_read_offset = input->v_offset;
        } else {
                /* I/O error */
                if (input->stream_errno == ESTALE && try_retry)
			*retry_r = TRUE;
		else {
			errno = input->stream_errno;
			mail_storage_set_critical(storage,
				"read(%s) failed: %m", uidlist->path);
		}
	}

	i_stream_destroy(&input);
	if (ret <= 0) {
		if (close(fd) < 0)
			i_error("close(%s) failed: %m", uidlist->path);
	}
	return ret;
}

static int
maildir_uidlist_has_changed(struct maildir_uidlist *uidlist, bool *recreated_r)
{
	struct mail_storage *storage = &uidlist->mbox->storage->storage;
        struct stat st;

	*recreated_r = FALSE;

	/* FIXME: nfs attribute cache flush */
	if (nfs_safe_stat(uidlist->path, &st) < 0) {
		if (errno != ENOENT) {
			mail_storage_set_critical(storage,
				"stat(%s) failed: %m", uidlist->path);
			return -1;
		}
		return 0;
	}

	if (st.st_ino != uidlist->fd_ino ||
	    !CMP_DEV_T(st.st_dev, uidlist->fd_dev)) {
		/* file recreated */
		*recreated_r = TRUE;
		return 1;
	} else if (st.st_size != uidlist->fd_size) {
		/* file modified but not recreated */
		return 1;
	} else {
		/* unchanged */
		return 0;
	}
}

int maildir_uidlist_refresh(struct maildir_uidlist *uidlist)
{
        unsigned int i;
        bool retry, recreated;
        int ret;

	if (uidlist->fd != -1) {
		ret = maildir_uidlist_has_changed(uidlist, &recreated);
		if (ret <= 0)
			return ret;

		if (recreated)
			maildir_uidlist_close(uidlist);
	}

        for (i = 0; ; i++) {
		ret = maildir_uidlist_update_read(uidlist, &retry,
						i < UIDLIST_ESTALE_RETRY_COUNT);
		if (!retry)
			break;
                /* ESTALE - try reopening and rereading */
        }
	if (ret >= 0)
		uidlist->initial_read = TRUE;
        return ret;
}

static const struct maildir_uidlist_rec *
maildir_uidlist_lookup_rec(struct maildir_uidlist *uidlist, uint32_t uid,
			   unsigned int *idx_r)
{
	struct maildir_uidlist_rec *const *recs;
	unsigned int idx, left_idx, right_idx;

	if (!uidlist->initial_read) {
		/* first time we need to read uidlist */
		if (maildir_uidlist_refresh(uidlist) < 0)
			return NULL;
	}

	idx = left_idx = 0;
	recs = array_get(&uidlist->records, &right_idx);
	while (left_idx < right_idx) {
		idx = (left_idx + right_idx) / 2;

		if (recs[idx]->uid < uid)
			left_idx = idx+1;
		else if (recs[idx]->uid > uid)
			right_idx = idx;
		else {
			*idx_r = idx;
			return recs[idx];
		}
	}

	if (idx > 0) idx--;
	*idx_r = idx;
	return NULL;
}

const char *
maildir_uidlist_lookup(struct maildir_uidlist *uidlist, uint32_t uid,
		       enum maildir_uidlist_rec_flag *flags_r)
{
	const struct maildir_uidlist_rec *rec;
	unsigned int idx;

	rec = maildir_uidlist_lookup_rec(uidlist, uid, &idx);
	if (rec == NULL) {
		if (uidlist->fd != -1)
			return NULL;

		/* the uidlist doesn't exist. */
		if (maildir_storage_sync_force(uidlist->mbox) < 0)
			return NULL;

		/* try again */
		rec = maildir_uidlist_lookup_rec(uidlist, uid, &idx);
		if (rec == NULL)
			return NULL;
	}

	*flags_r = rec->flags;
	return rec->filename;
}

bool maildir_uidlist_is_recent(struct maildir_uidlist *uidlist, uint32_t uid)
{
	enum maildir_uidlist_rec_flag flags;

	if (uidlist->first_recent_uid == 0 || uid < uidlist->first_recent_uid)
		return FALSE;

	if (maildir_uidlist_lookup(uidlist, uid, &flags) == NULL)
		return FALSE;

	i_assert(uidlist->first_recent_uid != uid ||
		 (flags & MAILDIR_UIDLIST_REC_FLAG_RECENT) != 0);
	return (flags & MAILDIR_UIDLIST_REC_FLAG_RECENT) != 0;
}

uint32_t maildir_uidlist_get_recent_count(struct maildir_uidlist *uidlist)
{
	struct maildir_uidlist_rec *const *recs;
	unsigned int idx, count;
	uint32_t recent_count;

	if (!uidlist->initial_sync) {
		/* we haven't synced yet, trust index */
		const struct mail_index_header *hdr;

		hdr = mail_index_get_header(uidlist->mbox->ibox.view);
		return hdr->recent_messages_count;
	}

	/* all recent messages were in new/ dir, so even if we did only
	   a partial sync we should know all the recent messages. */

	if (uidlist->first_recent_uid == 0)
		return 0;

	recs = array_get(&uidlist->records, &count);
	maildir_uidlist_lookup_rec(uidlist, uidlist->first_recent_uid, &idx);
	for (recent_count = 0; idx < count; idx++) {
		if ((recs[idx]->flags & MAILDIR_UIDLIST_REC_FLAG_RECENT) != 0)
			recent_count++;
	}
	return recent_count;
}

uint32_t maildir_uidlist_get_uid_validity(struct maildir_uidlist *uidlist)
{
	return uidlist->uid_validity;
}

uint32_t maildir_uidlist_get_next_uid(struct maildir_uidlist *uidlist)
{
	return !uidlist->initial_read ? 0 : uidlist->next_uid;
}

void maildir_uidlist_set_uid_validity(struct maildir_uidlist *uidlist,
				      uint32_t uid_validity)
{
	uidlist->uid_validity = uid_validity;
}

void maildir_uidlist_set_next_uid(struct maildir_uidlist *uidlist,
				  uint32_t next_uid, bool force)
{
	if (uidlist->next_uid < next_uid || force)
		uidlist->next_uid = next_uid;
}

static int maildir_uidlist_write_fd(struct maildir_uidlist *uidlist, int fd,
				    const char *path, unsigned int first_idx,
				    uoff_t *file_size_r)
{
	struct mail_storage *storage = &uidlist->mbox->storage->storage;
	struct maildir_uidlist_iter_ctx *iter;
	struct ostream *output;
	string_t *str;
	uint32_t uid;
        enum maildir_uidlist_rec_flag flags;
	const char *filename;
	int ret;

	i_assert(fd != -1);

	output = o_stream_create_file(fd, default_pool, 0, FALSE);
	str = t_str_new(512);

	if (output->offset == 0) {
		i_assert(first_idx == 0);
		uidlist->version = 1;
		if (uidlist->uid_validity == 0) {
			/* Get UIDVALIDITY from index */
			const struct mail_index_header *hdr;

			hdr = mail_index_get_header(uidlist->mbox->ibox.view);
			uidlist->uid_validity = hdr->uid_validity;
			i_assert(uidlist->uid_validity != 0);
		}

		str_printfa(str, "%u %u %u\n", uidlist->version,
			    uidlist->uid_validity, uidlist->next_uid);
		o_stream_send(output, str_data(str), str_len(str));
	} else {
		i_assert(first_idx != 0);
	}

	iter = maildir_uidlist_iter_init(uidlist->mbox->uidlist);
	iter->next += first_idx;

	while (maildir_uidlist_iter_next(iter, &uid, &flags, &filename)) {
		str_truncate(str, 0);
		str_printfa(str, "%u %s\n", uid, filename);
		o_stream_send(output, str_data(str), str_len(str));
	}
	maildir_uidlist_iter_deinit(iter);
	o_stream_flush(output);

	ret = output->stream_errno == 0 ? 0 : -1;

	*file_size_r = output->offset;
	o_stream_unref(&output);

	if (ret < 0) {
		mail_storage_set_critical(storage,
			"o_stream_send(%s) failed: %m", path);
		(void)close(fd);
		return -1;
	}

	if (!uidlist->mbox->ibox.fsync_disable) {
		if (fdatasync(fd) < 0) {
			mail_storage_set_critical(storage,
				"fsync(%s) failed: %m", path);
			(void)close(fd);
			return -1;
		}
	}
	return 0;
}

static int maildir_uidlist_recreate(struct maildir_uidlist *uidlist)
{
	struct maildir_mailbox *mbox = uidlist->mbox;
	const char *temp_path;
	struct stat st;
	mode_t old_mask;
	uoff_t file_size;
	int fd, ret;

	temp_path = t_strconcat(mbox->control_dir,
				"/" MAILDIR_UIDLIST_NAME ".tmp", NULL);

	old_mask = umask(0777 & ~mbox->mail_create_mode);
	fd = open(temp_path, O_RDWR | O_CREAT | O_TRUNC, 0777);
	umask(old_mask);

	if (fd == -1) {
		mail_storage_set_critical(&mbox->storage->storage,
			"open(%s, O_CREAT) failed: %m", temp_path);
		return -1;
	}

	if (mbox->mail_create_gid != (gid_t)-1) {
		if (fchown(fd, (uid_t)-1, mbox->mail_create_gid) < 0) {
			mail_storage_set_critical(&mbox->storage->storage,
				"fchown(%s) failed: %m", temp_path);
		}
	}

	ret = maildir_uidlist_write_fd(uidlist, fd, temp_path, 0, &file_size);
	if (ret == 0) {
		if (rename(temp_path, uidlist->path) < 0) {
			mail_storage_set_critical(&mbox->storage->storage,
				"rename(%s, %s) failed: %m",
				temp_path, uidlist->path);
			ret = -1;
		}
	}

	if (ret < 0) {
		if (unlink(temp_path) < 0) {
			mail_storage_set_critical(&mbox->storage->storage,
				"unlink(%s) failed: %m", temp_path);
		}
	} else if (fstat(fd, &st) < 0) {
		i_error("fstat(%s) failed: %m", temp_path);
		(void)close(fd);
		ret = -1;
	} else {
		i_assert(file_size == (uoff_t)st.st_size);
		uidlist->fd = fd;
		uidlist->fd_dev = st.st_dev;
		uidlist->fd_ino = st.st_ino;
		uidlist->fd_size = st.st_size;
		uidlist->last_read_offset = st.st_size;
	}
	return ret;
}

static int maildir_uidlist_update(struct maildir_uidlist_sync_ctx *ctx)
{
	struct maildir_uidlist *uidlist = ctx->uidlist;
	uoff_t file_size;

	if (ctx->uidlist->recreate || uidlist->fd == -1 ||
	    (uidlist->read_records_count + ctx->new_files_count) *
	    UIDLIST_COMPRESS_PERCENTAGE / 100 >= array_count(&uidlist->records))
		return maildir_uidlist_recreate(uidlist);

	i_assert(ctx->first_new_pos != 0);

	if (lseek(uidlist->fd, 0, SEEK_END) < 0) {
		mail_storage_set_critical(&uidlist->mbox->storage->storage,
			"lseek(%s) failed: %m", uidlist->path);
		return -1;
	}

	if (maildir_uidlist_write_fd(uidlist, uidlist->fd, uidlist->path,
				     ctx->first_new_pos, &file_size) < 0)
		return -1;

	uidlist->last_read_offset = file_size;
	return 0;
}

static void maildir_uidlist_mark_all(struct maildir_uidlist *uidlist,
				     bool nonsynced)
{
	struct maildir_uidlist_rec **recs;
	unsigned int i, count;

	recs = array_get_modifiable(&uidlist->records, &count);
	if (nonsynced) {
		for (i = 0; i < count; i++)
			recs[i]->flags |= MAILDIR_UIDLIST_REC_FLAG_NONSYNCED;
	} else {
		for (i = 0; i < count; i++)
			recs[i]->flags &= ~MAILDIR_UIDLIST_REC_FLAG_NONSYNCED;
	}
}

int maildir_uidlist_sync_init(struct maildir_uidlist *uidlist,
			      enum maildir_uidlist_sync_flags sync_flags,
			      struct maildir_uidlist_sync_ctx **sync_ctx_r)
{
	struct maildir_uidlist_sync_ctx *ctx;
	int ret;

	if ((ret = maildir_uidlist_lock(uidlist)) <= 0)
		return ret;

	*sync_ctx_r = ctx = i_new(struct maildir_uidlist_sync_ctx, 1);
	ctx->uidlist = uidlist;
	ctx->sync_flags = sync_flags;
	ctx->partial = (sync_flags & MAILDIR_UIDLIST_SYNC_PARTIAL) != 0;

	if (ctx->partial) {
		/* initially mark all nonsynced */
                maildir_uidlist_mark_all(uidlist, TRUE);
		return 1;
	}

	ctx->record_pool = pool_alloconly_create(MEMPOOL_GROWING
						 "maildir_uidlist_sync", 16384);
	ctx->files = hash_create(default_pool, ctx->record_pool, 4096,
				 maildir_filename_base_hash,
				 maildir_filename_base_cmp);

	i_array_init(&ctx->records, array_count(&uidlist->records));
	return 1;
}

static void
maildir_uidlist_sync_next_partial(struct maildir_uidlist_sync_ctx *ctx,
				  const char *filename,
				  enum maildir_uidlist_rec_flag flags)
{
	struct maildir_uidlist *uidlist = ctx->uidlist;
	struct maildir_uidlist_rec *rec;

	/* we'll update uidlist directly */
	rec = hash_lookup(uidlist->files, filename);
	i_assert(rec != NULL || UIDLIST_IS_LOCKED(uidlist));

	if (rec == NULL) {
		if (ctx->new_files_count == 0)
			ctx->first_new_pos = array_count(&uidlist->records);
		ctx->new_files_count++;
		ctx->changed = TRUE;

		if (uidlist->record_pool == NULL) {
			uidlist->record_pool =
				pool_alloconly_create(MEMPOOL_GROWING
						      "uidlist record_pool",
						      1024);
		}

		rec = p_new(uidlist->record_pool,
			    struct maildir_uidlist_rec, 1);
		rec->uid = (uint32_t)-1;
		array_append(&uidlist->records, &rec, 1);
		uidlist->change_counter++;
	}

	if ((flags & MAILDIR_UIDLIST_REC_FLAG_RECENT) != 0 &&
	    rec->uid != (uint32_t)-1)
		maildir_uidlist_mark_recent(uidlist, rec->uid);

	rec->flags = (rec->flags | flags) & ~MAILDIR_UIDLIST_REC_FLAG_NONSYNCED;
	rec->filename = p_strdup(uidlist->record_pool, filename);
	hash_insert(uidlist->files, rec->filename, rec);
}

int maildir_uidlist_sync_next_pre(struct maildir_uidlist_sync_ctx *ctx,
				  const char *filename)
{
	if (!UIDLIST_IS_LOCKED(ctx->uidlist) &&
	    hash_lookup(ctx->uidlist->files, filename) == NULL &&
	    (ctx->partial || hash_lookup(ctx->files, filename) == NULL)) {
		if (!ctx->uidlist->initial_read) {
			/* first time reading the uidlist */
			if (maildir_uidlist_refresh(ctx->uidlist) < 0) {
				ctx->failed = TRUE;
				return -1;
			}
			return maildir_uidlist_sync_next_pre(ctx, filename);
		}

		return 0;
	}

	return 1;
}

int maildir_uidlist_sync_next(struct maildir_uidlist_sync_ctx *ctx,
			      const char *filename,
			      enum maildir_uidlist_rec_flag flags)
{
	struct maildir_uidlist *uidlist = ctx->uidlist;
	struct maildir_uidlist_rec *rec, *old_rec;

	if (ctx->failed)
		return -1;

	if (ctx->partial) {
		maildir_uidlist_sync_next_partial(ctx, filename, flags);
		return 1;
	}

	rec = hash_lookup(ctx->files, filename);
	if (rec != NULL) {
		if ((rec->flags & (MAILDIR_UIDLIST_REC_FLAG_NEW_DIR |
				   MAILDIR_UIDLIST_REC_FLAG_MOVED)) == 0) {
			/* possibly duplicate */
			return 0;
		}

		rec->flags &= ~(MAILDIR_UIDLIST_REC_FLAG_NEW_DIR |
				MAILDIR_UIDLIST_REC_FLAG_MOVED);
	} else {
		old_rec = hash_lookup(uidlist->files, filename);
		i_assert(old_rec != NULL || UIDLIST_IS_LOCKED(uidlist));

		rec = p_new(ctx->record_pool, struct maildir_uidlist_rec, 1);

		if (old_rec != NULL)
			*rec = *old_rec;
		else {
			rec->uid = (uint32_t)-1;
			ctx->new_files_count++;
			ctx->changed = TRUE;
		}

		array_append(&ctx->records, &rec, 1);
	}

	if ((flags & MAILDIR_UIDLIST_REC_FLAG_RECENT) != 0 &&
	    rec->uid != (uint32_t)-1)
		maildir_uidlist_mark_recent(uidlist, rec->uid);

	rec->flags = (rec->flags | flags) & ~MAILDIR_UIDLIST_REC_FLAG_NONSYNCED;
	rec->filename = p_strdup(ctx->record_pool, filename);
	hash_insert(ctx->files, rec->filename, rec);
	return 1;
}

static int maildir_uid_cmp(const void *p1, const void *p2)
{
	const struct maildir_uidlist_rec *const *rec1 = p1, *const *rec2 = p2;

	return (*rec1)->uid < (*rec2)->uid ? -1 :
		(*rec1)->uid > (*rec2)->uid ? 1 : 0;
}

void maildir_uidlist_sync_remove(struct maildir_uidlist_sync_ctx *ctx,
				 const char *filename)
{
	struct maildir_uidlist_rec *const *recs, *const *pos, *rec;
	unsigned int count;

	i_assert(ctx->partial);

	if (ctx->first_new_pos != 0)
		ctx->first_new_pos--;

	rec = hash_lookup(ctx->uidlist->files, filename);
	i_assert(rec != NULL);

	hash_remove(ctx->uidlist->files, filename);

	recs = array_get(&ctx->uidlist->records, &count);
	pos = bsearch(&rec, recs, count, sizeof(*recs), maildir_uid_cmp);
	i_assert(pos != NULL);

	array_delete(&ctx->uidlist->records, pos - recs, 1);

	ctx->changed = TRUE;
	ctx->uidlist->recreate = TRUE;
}

const char *
maildir_uidlist_sync_get_full_filename(struct maildir_uidlist_sync_ctx *ctx,
				       const char *filename)
{
	struct maildir_uidlist_rec *rec;

	rec = hash_lookup(ctx->files, filename);
	return rec == NULL ? NULL : rec->filename;
}

const char *
maildir_uidlist_get_full_filename(struct maildir_uidlist *uidlist,
				  const char *filename)
{
	struct maildir_uidlist_rec *rec;

	rec = hash_lookup(uidlist->files, filename);
	return rec == NULL ? NULL : rec->filename;
}

static int maildir_time_cmp(const void *p1, const void *p2)
{
	const struct maildir_uidlist_rec *const *rec1 = p1, *const *rec2 = p2;

	return maildir_filename_sort_cmp((*rec1)->filename, (*rec2)->filename);
}

static void maildir_uidlist_assign_uids(struct maildir_uidlist_sync_ctx *ctx,
					unsigned int first_new_pos)
{
	struct maildir_uidlist_rec **recs;
	unsigned int dest, count;

	i_assert(UIDLIST_IS_LOCKED(ctx->uidlist));

	recs = array_get_modifiable(&ctx->uidlist->records, &count);

	/* sort new files and assign UIDs for them */
	qsort(recs + first_new_pos, count - first_new_pos,
	      sizeof(*recs), maildir_time_cmp);
	for (dest = first_new_pos; dest < count; dest++) {
		i_assert(recs[dest]->uid == (uint32_t)-1);
		recs[dest]->uid = ctx->uidlist->next_uid++;
		recs[dest]->flags &= ~MAILDIR_UIDLIST_REC_FLAG_MOVED;

		if ((recs[dest]->flags &
		     MAILDIR_UIDLIST_REC_FLAG_RECENT) != 0) {
			maildir_uidlist_mark_recent(ctx->uidlist,
						    recs[dest]->uid);
		}
	}

        ctx->uidlist->last_seen_uid = ctx->uidlist->next_uid-1;
}

static void maildir_uidlist_swap(struct maildir_uidlist_sync_ctx *ctx)
{
	struct maildir_uidlist *uidlist = ctx->uidlist;
	struct maildir_uidlist_rec **recs;
	unsigned int count;

	/* buffer is unsorted, sort it by UID */
	recs = array_get_modifiable(&ctx->records, &count);
	qsort(recs, count, sizeof(*recs), maildir_uid_cmp);

	array_free(&uidlist->records);
	uidlist->records = ctx->records;
	ctx->records.arr.buffer = NULL;

	hash_destroy(uidlist->files);
	uidlist->files = ctx->files;
	ctx->files = NULL;

	if (uidlist->record_pool != NULL)
		pool_unref(uidlist->record_pool);
	uidlist->record_pool = ctx->record_pool;
	ctx->record_pool = NULL;

	if (ctx->new_files_count != 0) {
		ctx->first_new_pos = count - ctx->new_files_count;
		maildir_uidlist_assign_uids(ctx, ctx->first_new_pos);
	}

	ctx->uidlist->change_counter++;
}

void maildir_uidlist_sync_finish(struct maildir_uidlist_sync_ctx *ctx)
{
	if (!ctx->partial) {
		if (!ctx->failed)
			maildir_uidlist_swap(ctx);
	} else {
		if (ctx->changed)
			maildir_uidlist_assign_uids(ctx, ctx->first_new_pos);
	}

	ctx->finished = TRUE;
	ctx->uidlist->initial_sync = TRUE;
}

int maildir_uidlist_sync_deinit(struct maildir_uidlist_sync_ctx **_ctx)
{
	struct maildir_uidlist_sync_ctx *ctx = *_ctx;
	int ret = ctx->failed ? -1 : 0;

	i_assert(ctx->uidlist->uid_validity != 0);

	*_ctx = NULL;

	if (!ctx->finished)
		maildir_uidlist_sync_finish(ctx);

	if (ctx->partial)
		maildir_uidlist_mark_all(ctx->uidlist, FALSE);

	if (ctx->changed && !ctx->failed) {
		t_push();
		ret = maildir_uidlist_update(ctx);
		t_pop();
	}

	maildir_uidlist_unlock(ctx->uidlist);

	if (ctx->files != NULL)
		hash_destroy(ctx->files);
	if (ctx->record_pool != NULL)
		pool_unref(ctx->record_pool);
	if (array_is_created(&ctx->records))
		array_free(&ctx->records);
	i_free(ctx);
	return ret;
}

void maildir_uidlist_add_flags(struct maildir_uidlist *uidlist,
			       const char *filename,
			       enum maildir_uidlist_rec_flag flags)
{
	struct maildir_uidlist_rec *rec;

	rec = hash_lookup(uidlist->files, filename);
	i_assert(rec != NULL);

	rec->flags |= flags;
}

struct maildir_uidlist_iter_ctx *
maildir_uidlist_iter_init(struct maildir_uidlist *uidlist)
{
	struct maildir_uidlist_iter_ctx *ctx;
	unsigned int count;

	ctx = i_new(struct maildir_uidlist_iter_ctx, 1);
	ctx->uidlist = uidlist;
	ctx->next = array_get(&uidlist->records, &count);
	ctx->end = ctx->next + count;
	ctx->change_counter = ctx->uidlist->change_counter;
	return ctx;
}

static void
maildir_uidlist_iter_update_idx(struct maildir_uidlist_iter_ctx *ctx)
{
	unsigned int old_rev_idx, idx, count;

	old_rev_idx = ctx->end - ctx->next;
	ctx->next = array_get(&ctx->uidlist->records, &count);
	ctx->end = ctx->next + count;

	idx = old_rev_idx >= count ? 0 :
		count - old_rev_idx;
	while (idx < count && ctx->next[idx]->uid <= ctx->prev_uid)
		idx++;
	while (idx > 0 && ctx->next[idx-1]->uid > ctx->prev_uid)
		idx--;

	ctx->next += idx;
}

int maildir_uidlist_iter_next(struct maildir_uidlist_iter_ctx *ctx,
			      uint32_t *uid_r,
			      enum maildir_uidlist_rec_flag *flags_r,
			      const char **filename_r)
{
	struct maildir_uidlist_rec *rec;

	if (ctx->change_counter != ctx->uidlist->change_counter)
		maildir_uidlist_iter_update_idx(ctx);

	if (ctx->next == ctx->end)
		return 0;

	rec = *ctx->next;
	i_assert(rec->uid != (uint32_t)-1);

	ctx->prev_uid = rec->uid;
	ctx->next++;

	*uid_r = rec->uid;
	*flags_r = rec->flags;
	*filename_r = rec->filename;
	return 1;
}

void maildir_uidlist_iter_deinit(struct maildir_uidlist_iter_ctx *ctx)
{
	i_free(ctx);
}
