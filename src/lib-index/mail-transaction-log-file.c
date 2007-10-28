/* Copyright (c) 2003-2007 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "buffer.h"
#include "file-dotlock.h"
#include "nfs-workarounds.h"
#include "read-full.h"
#include "write-full.h"
#include "mmap-util.h"
#include "mail-index-private.h"
#include "mail-transaction-log-private.h"

#define LOG_PREFETCH 1024
#define MEMORY_LOG_NAME "(in-memory transaction log file)"

void
mail_transaction_log_file_set_corrupted(struct mail_transaction_log_file *file,
					const char *fmt, ...)
{
	va_list va;

	file->corrupted = TRUE;
	file->hdr.indexid = 0;
	if (!MAIL_TRANSACTION_LOG_FILE_IN_MEMORY(file)) {
		/* indexid=0 marks the log file as corrupted */
		if (pwrite_full(file->fd, &file->hdr.indexid,
				sizeof(file->hdr.indexid),
				offsetof(struct mail_transaction_log_header,
					 indexid)) < 0) {
			mail_index_file_set_syscall_error(file->log->index,
				file->filepath, "pwrite()");
		}
	}

	va_start(va, fmt);
	t_push();
	mail_index_set_error(file->log->index,
			     "Corrupted transaction log file %s: %s",
			     file->filepath, t_strdup_vprintf(fmt, va));
	t_pop();
	va_end(va);
}

struct mail_transaction_log_file *
mail_transaction_log_file_alloc(struct mail_transaction_log *log,
				const char *path)
{
	struct mail_transaction_log_file *file;

	file = i_new(struct mail_transaction_log_file, 1);
	file->log = log;
	file->filepath = i_strdup(path);
	file->fd = -1;
	return file;
}

void mail_transaction_log_file_free(struct mail_transaction_log_file **_file)
{
	struct mail_transaction_log_file *file = *_file;
	struct mail_transaction_log_file **p;
	int old_errno = errno;

	*_file = NULL;

	mail_transaction_log_file_unlock(file);

	for (p = &file->log->files; *p != NULL; p = &(*p)->next) {
		if (*p == file) {
			*p = file->next;
			break;
		}
	}

	if (file == file->log->head)
		file->log->head = NULL;

	if (file->buffer != NULL) 
		buffer_free(&file->buffer);

	if (file->mmap_base != NULL) {
		if (munmap(file->mmap_base, file->mmap_size) < 0) {
			mail_index_file_set_syscall_error(file->log->index,
							  file->filepath,
							  "munmap()");
		}
	}

	if (file->fd != -1) {
		if (close(file->fd) < 0) {
			mail_index_file_set_syscall_error(file->log->index,
							  file->filepath,
							  "close()");
		}
	}

	i_free(file->filepath);
        i_free(file);

        errno = old_errno;
}

static void
mail_transaction_log_file_add_to_list(struct mail_transaction_log_file *file)
{
	struct mail_transaction_log *log = file->log;
	struct mail_transaction_log_file **p;
	struct mail_index_map *map = log->index->map;

	if (map != NULL && file->hdr.file_seq == map->hdr.log_file_seq &&
	    map->hdr.log_file_head_offset != 0) {
		/* we can get a valid log offset from index file. initialize
		   sync_offset from it so we don't have to read the whole log
		   file from beginning. */
		if (map->hdr.log_file_head_offset >= file->hdr.hdr_size)
			file->sync_offset = map->hdr.log_file_head_offset;
		else {
			mail_index_set_error(log->index,
				"%s: log_file_head_offset too small",
				log->index->filepath);
			file->sync_offset = file->hdr.hdr_size;
		}
		file->saved_tail_offset = map->hdr.log_file_tail_offset;
	} else {
		file->sync_offset = file->hdr.hdr_size;
	}

	/* insert it to correct position */
	for (p = &log->files; *p != NULL; p = &(*p)->next) {
		if ((*p)->hdr.file_seq > file->hdr.file_seq)
			break;
		i_assert((*p)->hdr.file_seq < file->hdr.file_seq);
	}

	file->next = *p;
	*p = file;
}

static int
mail_transaction_log_init_hdr(struct mail_transaction_log *log,
			      struct mail_transaction_log_header *hdr)
{
	struct mail_index *index = log->index;

	memset(hdr, 0, sizeof(*hdr));
	hdr->major_version = MAIL_TRANSACTION_LOG_MAJOR_VERSION;
	hdr->minor_version = MAIL_TRANSACTION_LOG_MINOR_VERSION;
	hdr->hdr_size = sizeof(struct mail_transaction_log_header);
	hdr->indexid = log->index->indexid;
	hdr->create_stamp = ioloop_time;

	if (index->fd != -1) {
		/* not creating index - make sure we have latest header */
		if (!index->mapping) {
			if (mail_index_map(index,
					   MAIL_INDEX_SYNC_HANDLER_HEAD) <= 0)
				return -1;
		} else {
			/* if we got here from mapping, the .log file is
			   corrupted. use whatever values we got from index
			   file */
		}
	}
	if (index->map != NULL) {
		hdr->prev_file_seq = index->map->hdr.log_file_seq;
		hdr->prev_file_offset = index->map->hdr.log_file_head_offset;
		hdr->file_seq = index->map->hdr.log_file_seq + 1;
	} else {
		hdr->file_seq = 1;
	}

	if (log->head != NULL && hdr->file_seq <= log->head->hdr.file_seq) {
		/* make sure the sequence grows */
		hdr->file_seq = log->head->hdr.file_seq+1;
	}
	return 0;
}

struct mail_transaction_log_file *
mail_transaction_log_file_alloc_in_memory(struct mail_transaction_log *log)
{
	struct mail_transaction_log_file *file;

	file = mail_transaction_log_file_alloc(log, MEMORY_LOG_NAME);
	if (mail_transaction_log_init_hdr(log, &file->hdr) < 0) {
		i_free(file);
		return NULL;
	}

	file->buffer = buffer_create_dynamic(default_pool, 4096);
	file->buffer_offset = sizeof(file->hdr);

	mail_transaction_log_file_add_to_list(file);
	return file;
}

static int
mail_transaction_log_file_dotlock(struct mail_transaction_log_file *file)
{
	int ret;

	if (file->log->dotlock_count > 0)
		ret = 1;
	else {
		ret = file_dotlock_create(&file->log->dotlock_settings,
					  file->filepath, 0,
					  &file->log->dotlock);
	}
	if (ret > 0) {
		file->log->dotlock_count++;
		file->locked = TRUE;
		return 0;
	}
	if (ret < 0) {
		mail_index_file_set_syscall_error(file->log->index,
						  file->filepath,
						  "file_dotlock_create()");
		return -1;
	}

	mail_index_set_error(file->log->index,
			     "Timeout while waiting for release of "
			     "dotlock for transaction log file %s",
			     file->filepath);
	file->log->index->index_lock_timeout = TRUE;
	return -1;
}

static int
mail_transaction_log_file_undotlock(struct mail_transaction_log_file *file)
{
	int ret;

	if (--file->log->dotlock_count > 0)
		return 0;

	ret = file_dotlock_delete(&file->log->dotlock);
	if (ret < 0) {
		mail_index_file_set_syscall_error(file->log->index,
			file->filepath, "file_dotlock_delete()");
		return -1;
	}

	if (ret == 0) {
		mail_index_set_error(file->log->index,
			"Dotlock was lost for transaction log file %s",
			file->filepath);
		return -1;
	}
	return 0;
}

int mail_transaction_log_file_lock(struct mail_transaction_log_file *file)
{
	int ret;

	if (file->locked)
		return 0;

	if (MAIL_TRANSACTION_LOG_FILE_IN_MEMORY(file)) {
		file->locked = TRUE;
		return 0;
	}

	if (file->log->index->lock_method == FILE_LOCK_METHOD_DOTLOCK)
		return mail_transaction_log_file_dotlock(file);

	i_assert(file->file_lock == NULL);
	ret = mail_index_lock_fd(file->log->index, file->filepath, file->fd,
				 F_WRLCK, MAIL_TRANSCATION_LOG_LOCK_TIMEOUT,
				 &file->file_lock);
	if (ret > 0) {
		file->locked = TRUE;
		return 0;
	}
	if (ret < 0) {
		mail_index_file_set_syscall_error(file->log->index,
						  file->filepath,
						  "mail_index_wait_lock_fd()");
		return -1;
	}

	mail_index_set_error(file->log->index,
		"Timeout while waiting for lock for transaction log file %s",
		file->filepath);
	file->log->index->index_lock_timeout = TRUE;
	return -1;
}

void mail_transaction_log_file_unlock(struct mail_transaction_log_file *file)
{
	if (!file->locked)
		return;

	file->locked = FALSE;

	if (MAIL_TRANSACTION_LOG_FILE_IN_MEMORY(file))
		return;

	if (file->log->index->lock_method == FILE_LOCK_METHOD_DOTLOCK) {
		mail_transaction_log_file_undotlock(file);
		return;
	}

	file_unlock(&file->file_lock);
}

static int
mail_transaction_log_file_read_hdr(struct mail_transaction_log_file *file,
				   bool ignore_estale)
{
        struct mail_transaction_log_file *f;
	int ret;

	i_assert(!MAIL_TRANSACTION_LOG_FILE_IN_MEMORY(file));

	if (file->corrupted)
		return 0;

	ret = pread_full(file->fd, &file->hdr, sizeof(file->hdr), 0);
	if (ret < 0) {
                if (errno != ESTALE || !ignore_estale) {
                        mail_index_file_set_syscall_error(file->log->index,
                                                          file->filepath,
                                                          "pread_full()");
                }
		return -1;
	}
	if (ret == 0) {
		mail_transaction_log_file_set_corrupted(file,
			"unexpected end of file while reading header");
		return 0;
	}

	if (file->hdr.major_version != MAIL_TRANSACTION_LOG_MAJOR_VERSION) {
		/* incompatible version - fix silently */
		return 0;
	}
	if (file->hdr.hdr_size < MAIL_TRANSACTION_LOG_HEADER_MIN_SIZE) {
		mail_transaction_log_file_set_corrupted(file,
			"Header size too small");
		return 0;
	}
	if (file->hdr.hdr_size < sizeof(file->hdr)) {
		/* @UNSAFE: smaller than we expected - zero out the fields we
		   shouldn't have filled */
		memset(PTR_OFFSET(&file->hdr, file->hdr.hdr_size), 0,
		       sizeof(file->hdr) - file->hdr.hdr_size);
	}

	if (file->hdr.indexid == 0) {
		/* corrupted */
		file->corrupted = TRUE;
		mail_index_set_error(file->log->index,
			"Transaction log file %s: marked corrupted",
			file->filepath);
		return 0;
	}
	if (file->hdr.indexid != file->log->index->indexid) {
		if (file->log->index->indexid != 0) {
			/* index file was probably just rebuilt and we don't
			   know about it yet */
			mail_transaction_log_file_set_corrupted(file,
				"indexid changed %u -> %u",
				file->log->index->indexid, file->hdr.indexid);
			return 0;
		}

		/* creating index file. since transaction log is created
		   first, use the indexid in it to create the main index
		   to avoid races. */
		file->log->index->indexid = file->hdr.indexid;
	}

	/* make sure we already don't have a file with the same sequence
	   opened. it shouldn't happen unless the old log file was
	   corrupted. */
	for (f = file->log->files; f != NULL; f = f->next) {
		if (f->hdr.file_seq == file->hdr.file_seq) {
			/* mark the old file corrupted. we can't safely remove
			   it from the list however, so return failure. */
			mail_transaction_log_file_set_corrupted(f,
				"duplicate transaction log sequence (%u)",
				f->hdr.file_seq);
			return 0;
		}
	}

	return 1;
}

static int
mail_transaction_log_file_stat(struct mail_transaction_log_file *file,
			       bool ignore_estale)
{
	struct stat st;

	if (fstat(file->fd, &st) < 0) {
                if (errno != ESTALE || !ignore_estale) {
			mail_index_file_set_syscall_error(file->log->index,
				file->filepath, "fstat()");
                }
		return -1;
	}

	file->st_dev = st.st_dev;
	file->st_ino = st.st_ino;
	file->last_mtime = st.st_mtime;
	file->last_size = st.st_size;
	return 0;
}

static bool
mail_transaction_log_file_is_dupe(struct mail_transaction_log_file *file)
{
	struct mail_transaction_log_file *tmp;

	for (tmp = file->log->files; tmp != NULL; tmp = tmp->next) {
		if (tmp->st_ino == file->st_ino &&
		    CMP_DEV_T(tmp->st_dev, file->st_dev))
			return TRUE;
	}
	return FALSE;
}

static int
mail_transaction_log_file_create2(struct mail_transaction_log_file *file,
				  int new_fd, bool reset,
				  struct dotlock **dotlock)
{
	struct mail_index *index = file->log->index;
	struct stat st;
	const char *path2;
	int fd, ret;
	bool rename_existing;

	if (index->nfs_flush)
		nfs_flush_attr_cache(file->filepath);

	/* log creation is locked now - see if someone already created it.
	   note that if we're rotating, we need to keep the log locked until
	   the file has been rewritten. and because fcntl() locks are stupid,
	   if we go and open()+close() the file and we had it already opened,
	   its locks are lost. so we use stat() to check if the file has been
	   recreated, although it almost never is. */
	if (reset)
		rename_existing = FALSE;
	else if (nfs_safe_stat(file->filepath, &st) < 0) {
		if (errno != ENOENT) {
			mail_index_file_set_syscall_error(index, file->filepath,
							  "stat()");
			return -1;
		}
		rename_existing = FALSE;
	} else if (st.st_ino == file->st_ino &&
		   CMP_DEV_T(st.st_dev, file->st_dev) &&
		   /* inode/dev checks are enough when we're rotating the file,
		      but not when we're replacing a broken log file */
		   st.st_mtime == file->last_mtime &&
		   (uoff_t)st.st_size == file->last_size) {
		/* no-one else recreated the file */
		rename_existing = TRUE;
	} else {
		/* recreated. use the file if its header is ok */
		fd = nfs_safe_open(file->filepath, O_RDWR);
		if (fd == -1) {
			if (errno != ENOENT) {
				mail_index_file_set_syscall_error(index,
					file->filepath, "open()");
				return -1;
			}
		} else {
			file->fd = fd;
			if (mail_transaction_log_file_read_hdr(file,
							       FALSE) > 0 &&
			    mail_transaction_log_file_stat(file, FALSE) == 0) {
				/* yes, it was ok */
				(void)file_dotlock_delete(dotlock);
				mail_transaction_log_file_add_to_list(file);
				return 0;
			}
			file->fd = -1;
			if (close(fd) < 0) {
				mail_index_file_set_syscall_error(index,
					file->filepath, "close()");
			}
		}
		rename_existing = FALSE;
	}

	if (mail_transaction_log_init_hdr(file->log, &file->hdr) < 0)
		return -1;

	if (reset) {
		file->hdr.prev_file_seq = 0;
		file->hdr.prev_file_offset = 0;
	}

	if (write_full(new_fd, &file->hdr, sizeof(file->hdr)) < 0) {
		mail_index_file_set_syscall_error(index, file->filepath,
						  "write_full()");
		return -1;
	}

	if (index->nfs_flush) {
		/* the header isn't important, so don't bother calling
		   fdatasync() unless NFS is used */
		if (fdatasync(new_fd) < 0) {
			mail_index_file_set_syscall_error(index, file->filepath,
							  "fdatasync()");
			return -1;
		}
	}

	file->fd = new_fd;
	ret = mail_transaction_log_file_stat(file, FALSE);

	/* if we return -1 the dotlock deletion code closes the fd */
	file->fd = -1;
	if (ret < 0)
		return -1;

	/* keep two log files */
	if (rename_existing) {
		/* rename() would be nice and easy way to do this, except then
		   there's a race condition between the rename and
		   file_dotlock_replace(). during that time the log file
		   doesn't exist, which could cause problems. */
		path2 = t_strconcat(file->filepath, ".2", NULL);
		if (unlink(path2) < 0 && errno != ENOENT) {
                        mail_index_set_error(index, "unlink(%s) failed: %m",
					     path2);
			/* try to link() anyway */
		}
		if (link(file->filepath, path2) < 0 &&
		    errno != ENOENT && errno != EEXIST) {
                        mail_index_set_error(index, "link(%s, %s) failed: %m",
					     file->filepath, path2);
			/* ignore the error. we don't care that much about the
			   second log file and we're going to overwrite this
			   first one. */
		}
	}

	if (file_dotlock_replace(dotlock,
				 DOTLOCK_REPLACE_FLAG_DONT_CLOSE_FD) <= 0)
		return -1;

	/* success */
	file->fd = new_fd;
        mail_transaction_log_file_add_to_list(file);
	return 0;
}

int mail_transaction_log_file_create(struct mail_transaction_log_file *file,
				     bool reset)
{
	struct mail_index *index = file->log->index;
	struct dotlock *dotlock;
	mode_t old_mask;
	int fd;

	i_assert(!MAIL_INDEX_IS_IN_MEMORY(index));

	/* With dotlocking we might already have path.lock created, so this
	   filename has to be different. */
	old_mask = umask(index->mode ^ 0666);
	fd = file_dotlock_open(&file->log->new_dotlock_settings,
			       file->filepath, 0, &dotlock);
	umask(old_mask);

	if (fd == -1) {
		mail_index_file_set_syscall_error(index, file->filepath,
						  "file_dotlock_open()");
		return -1;
	}

	if (index->gid != (gid_t)-1 &&
	    fchown(fd, (uid_t)-1, index->gid) < 0) {
		mail_index_file_set_syscall_error(index, file->filepath,
						  "fchown()");
		(void)file_dotlock_delete(&dotlock);
		return -1;
	}

        /* either fd gets used or the dotlock gets deleted and returned fd
           is for the existing file */
        if (mail_transaction_log_file_create2(file, fd, reset, &dotlock) < 0) {
		if (dotlock != NULL)
			(void)file_dotlock_delete(&dotlock);
		return -1;
	}
	return 0;
}

int mail_transaction_log_file_open(struct mail_transaction_log_file *file,
				   bool check_existing)
{
        unsigned int i;
	bool ignore_estale;
	int ret;

        for (i = 0;; i++) {
                file->fd = nfs_safe_open(file->filepath, O_RDWR);
                if (file->fd == -1) {
			if (errno == ENOENT)
				return 0;

			mail_index_file_set_syscall_error(file->log->index,
				file->filepath, "open()");
			return -1;
                }

		ignore_estale = i < MAIL_INDEX_ESTALE_RETRY_COUNT;
		if (mail_transaction_log_file_stat(file, ignore_estale) < 0)
			ret = -1;
		else if (check_existing &&
			 mail_transaction_log_file_is_dupe(file))
			return 0;
		else {
			ret = mail_transaction_log_file_read_hdr(file,
								 ignore_estale);
		}
		if (ret > 0) {
			/* success */
			break;
		}

		if (ret == 0) {
			/* corrupted */
			if (unlink(file->filepath) < 0 && errno != ENOENT) {
				mail_index_set_error(file->log->index,
						     "unlink(%s) failed: %m",
						     file->filepath);
			}
			return 0;
		}
		if (errno != ESTALE ||
		    i == MAIL_INDEX_ESTALE_RETRY_COUNT) {
			/* syscall error */
			return -1;
		}

		/* ESTALE - try again */
        }

	mail_transaction_log_file_add_to_list(file);
	return 1;
}

static int
log_file_track_mailbox_sync_offset_hdr(struct mail_transaction_log_file *file,
				       const void *data, unsigned int size)
{
	const struct mail_transaction_header_update *u = data;
	const struct mail_index_header *ihdr;
	const unsigned int offset_pos =
		offsetof(struct mail_index_header, log_file_tail_offset);
	const unsigned int offset_size = sizeof(ihdr->log_file_tail_offset);
	uint32_t sync_offset;

	i_assert(offset_size == sizeof(sync_offset));

	if (size < sizeof(*u) || size < sizeof(*u) + u->size) {
		mail_transaction_log_file_set_corrupted(file,
			"header update extends beyond record size");
		return -1;
	}

	if (u->offset <= offset_pos &&
	    u->offset + u->size >= offset_pos + offset_size) {
		memcpy(&sync_offset,
		       CONST_PTR_OFFSET(u + 1, offset_pos - u->offset),
		       sizeof(sync_offset));

		if (sync_offset < file->saved_tail_offset) {
			mail_transaction_log_file_set_corrupted(file,
				"log_file_tail_offset shrank");
			return -1;
		}
		file->saved_tail_offset = sync_offset;
		if (sync_offset > file->max_tail_offset)
			file->max_tail_offset = sync_offset;
		return 1;
	}
	return 0;
}

static int
log_file_track_mailbox_sync_offset(struct mail_transaction_log_file *file,
				   const struct mail_transaction_header *hdr,
				   unsigned int trans_size)
{
	int ret;

	i_assert((hdr->type & MAIL_TRANSACTION_EXTERNAL) != 0);

	if ((hdr->type & MAIL_TRANSACTION_TYPE_MASK) ==
	    MAIL_TRANSACTION_HEADER_UPDATE) {
		/* see if this updates mailbox_sync_offset */
		ret = log_file_track_mailbox_sync_offset_hdr(file, hdr + 1,
							     trans_size -
							     sizeof(*hdr));
		if (ret != 0)
			return ret < 0 ? -1 : 0;
	}

	if (file->max_tail_offset == file->sync_offset) {
		/* external transactions aren't synced to mailbox. we can
		   update mailbox sync offset to skip this transaction to
		   avoid re-reading it at the next sync. */
		file->max_tail_offset += trans_size;
	}
	return 0;
}

static int
mail_transaction_log_file_sync(struct mail_transaction_log_file *file)
{
        const struct mail_transaction_header *hdr;
	const void *data;
	struct stat st;
	size_t size, avail;
	uint32_t trans_size = 0;

	data = buffer_get_data(file->buffer, &size);

	if (file->sync_offset < file->buffer_offset)
		file->sync_offset = file->buffer_offset;

	while (file->sync_offset - file->buffer_offset + sizeof(*hdr) <= size) {
		hdr = CONST_PTR_OFFSET(data, file->sync_offset -
				       file->buffer_offset);
		trans_size = mail_index_offset_to_uint32(hdr->size);
		if (trans_size == 0) {
			/* unfinished */
			return 1;
		}
		if (trans_size < sizeof(*hdr)) {
			mail_transaction_log_file_set_corrupted(file,
				"hdr.size too small (%u)", trans_size);
			return -1;
		}

		if (file->sync_offset - file->buffer_offset + trans_size > size)
			break;

		/* transaction has been fully written */
		if ((hdr->type & MAIL_TRANSACTION_EXTERNAL) != 0) {
			if (log_file_track_mailbox_sync_offset(file, hdr,
							       trans_size) < 0)
				return -1;
		}
		file->sync_offset += trans_size;
		trans_size = 0;
	}

	if (file->mmap_base != NULL && !file->locked) {
		/* Now that all the mmaped pages have page faulted, check if
		   the file had changed while doing that. Only after the last
		   page has faulted, the size returned by fstat() can be
		   trusted. Otherwise it might point to a page boundary while
		   the next page is still being written.

		   Without this check we might see partial transactions,
		   sometimes causing "Extension record updated without intro
		   prefix" errors. */
		if (fstat(file->fd, &st) < 0) {
			mail_index_file_set_syscall_error(file->log->index,
							  file->filepath,
							  "fstat()");
			return -1;
		}
		if ((uoff_t)st.st_size != file->last_size) {
			file->last_size = st.st_size;
			return 0;
		}
	}

	avail = file->sync_offset - file->buffer_offset;
	if (avail != size) {
		/* There's more data than we could sync at the moment. If the
		   last record's size wasn't valid, we can't know if it will
		   be updated unless we've locked the log.

		   If the record size was valid, this is an error because the
		   pread()s or the above fstat() check for mmaps should have
		   guaranteed that this doesn't happen. */
		if (file->locked || trans_size != 0) {
			if (trans_size != 0) {
				mail_transaction_log_file_set_corrupted(file,
					"hdr.size too large (%u)", trans_size);
			} else {
				mail_transaction_log_file_set_corrupted(file,
					"Unexpected garbage at EOF");
			}
			return -1;
		}

		if (file->log->index->nfs_flush) {
			/* The size field will be updated soon */
			nfs_flush_read_cache(file->filepath, file->fd,
					     F_UNLCK, FALSE);
		}
	}

	if (file->next != NULL &&
	    file->hdr.file_seq == file->next->hdr.prev_file_seq &&
	    file->next->hdr.prev_file_offset != file->sync_offset) {
		mail_index_set_error(file->log->index,
			"Invalid transaction log size "
			"(%"PRIuUOFF_T" vs %u): %s", file->sync_offset,
			file->log->head->hdr.prev_file_offset, file->filepath);
		return -1;
	}

	return 1;
}

static int
mail_transaction_log_file_insert_read(struct mail_transaction_log_file *file,
				      uoff_t offset)
{
	void *data;
	size_t size;
	ssize_t ret;

	size = file->buffer_offset - offset;
	buffer_copy(file->buffer, size, file->buffer, 0, (size_t)-1);

	data = buffer_get_space_unsafe(file->buffer, 0, size);
	ret = pread_full(file->fd, data, size, offset);
	if (ret > 0) {
		/* success */
		file->buffer_offset -= size;
		return 1;
	}

	/* failure. don't leave ourself to inconsistent state */
	buffer_copy(file->buffer, 0, file->buffer, size, (size_t)-1);
	buffer_set_used_size(file->buffer, file->buffer->used - size);

	if (ret == 0) {
		mail_transaction_log_file_set_corrupted(file, "file shrank");
		return 0;
	} else if (errno == ESTALE) {
		/* log file was deleted in NFS server, fail silently */
		return 0;
	} else {
		mail_index_file_set_syscall_error(file->log->index,
						  file->filepath, "pread()");
		return -1;
	}
}

static int
mail_transaction_log_file_read_more(struct mail_transaction_log_file *file)
{
	void *data;
	size_t size;
	uint32_t read_offset;
	ssize_t ret;

	read_offset = file->buffer_offset + buffer_get_used_size(file->buffer);

	do {
		data = buffer_append_space_unsafe(file->buffer, LOG_PREFETCH);
		ret = pread(file->fd, data, LOG_PREFETCH, read_offset);
		if (ret > 0)
			read_offset += ret;

		size = read_offset - file->buffer_offset;
		buffer_set_used_size(file->buffer, size);
	} while (ret > 0 || (ret < 0 && errno == EINTR));

	file->last_size = read_offset;

	if (ret < 0) {
		if (errno == ESTALE) {
			/* log file was deleted in NFS server, fail silently */
			return 0;
		}

		mail_index_file_set_syscall_error(file->log->index,
						  file->filepath, "pread()");
		return -1;
	}
	return 1;
}

static int
mail_transaction_log_file_read(struct mail_transaction_log_file *file,
			       uoff_t start_offset)
{
	int ret;

	i_assert(file->mmap_base == NULL);

	if (file->log->index->nfs_flush) {
		/* Make sure we know the latest file size */
		nfs_flush_attr_cache_fd(file->filepath, file->fd);
	}

	if (file->buffer != NULL && file->buffer_offset > start_offset) {
		/* we have to insert missing data to beginning of buffer */
		ret = mail_transaction_log_file_insert_read(file, start_offset);
		if (ret <= 0)
			return ret;
	}

	if (file->buffer == NULL) {
		file->buffer =
			buffer_create_dynamic(default_pool, LOG_PREFETCH);
		file->buffer_offset = start_offset;
	}

	if ((ret = mail_transaction_log_file_read_more(file)) <= 0)
		return ret;

	if ((ret = mail_transaction_log_file_sync(file)) <= 0) {
		i_assert(ret != 0); /* happens only with mmap */
		return -1;
	}

	i_assert(file->sync_offset >= file->buffer_offset);
	buffer_set_used_size(file->buffer,
			     file->sync_offset - file->buffer_offset);
	return 1;
}

static int
log_file_map_check_offsets(struct mail_transaction_log_file *file,
			   uoff_t start_offset, uoff_t end_offset)
{
	if (start_offset > file->sync_offset) {
		/* broken start offset */
		mail_index_set_error(file->log->index,
			"%s: start_offset (%"PRIuUOFF_T") > "
			"current sync_offset (%"PRIuUOFF_T")",
			file->filepath, start_offset, file->sync_offset);
		return 0;
	}
	if (end_offset != (uoff_t)-1 && end_offset > file->sync_offset) {
		mail_index_set_error(file->log->index,
			"%s: end_offset (%"PRIuUOFF_T") > "
			"current sync_offset (%"PRIuUOFF_T")",
			file->filepath, start_offset, file->sync_offset);
		return 0;
	}

	return 1;
}

static int
mail_transaction_log_file_mmap(struct mail_transaction_log_file *file)
{
	if (file->buffer != NULL) {
		/* in case we just switched to mmaping */
		buffer_free(&file->buffer);
	}
	file->mmap_size = file->last_size;
	file->mmap_base = mmap(NULL, file->mmap_size, PROT_READ, MAP_SHARED,
			       file->fd, 0);
	if (file->mmap_base == MAP_FAILED) {
		file->mmap_base = NULL;
		file->mmap_size = 0;
		mail_index_file_set_syscall_error(file->log->index,
						  file->filepath, "mmap()");
		return -1;
	}

	if (file->mmap_size > mmap_get_page_size()) {
		if (madvise(file->mmap_base, file->mmap_size,
			    MADV_SEQUENTIAL) < 0) {
			mail_index_file_set_syscall_error(file->log->index,
				file->filepath, "madvise()");
		}
	}

	file->buffer = buffer_create_const_data(default_pool,
						file->mmap_base,
						file->mmap_size);
	file->buffer_offset = 0;
	return 0;
}

static void
mail_transaction_log_file_munmap(struct mail_transaction_log_file *file)
{
	if (file->mmap_base == NULL)
		return;

	if (munmap(file->mmap_base, file->mmap_size) < 0) {
		mail_index_file_set_syscall_error(file->log->index,
						  file->filepath, "munmap()");
	}
	file->mmap_base = NULL;
	file->mmap_size = 0;
	buffer_free(&file->buffer);
}

int mail_transaction_log_file_map(struct mail_transaction_log_file *file,
				  uoff_t start_offset, uoff_t end_offset)
{
	struct mail_index *index = file->log->index;
	size_t size;
	struct stat st;
	int ret;

	if (file->hdr.indexid == 0) {
		/* corrupted */
		return 0;
	}

	i_assert(start_offset >= file->hdr.hdr_size);
	i_assert(start_offset <= end_offset);

	if (index->log_locked && file == file->log->head &&
	    end_offset == (uoff_t)-1) {
		/* we're not interested of going further than sync_offset */
		if (log_file_map_check_offsets(file, start_offset,
					       end_offset) == 0)
			return 0;
		i_assert(start_offset <= file->sync_offset);
		end_offset = file->sync_offset;
	}

	if (file->buffer != NULL && file->buffer_offset <= start_offset) {
		/* see if we already have it */
		size = buffer_get_used_size(file->buffer);
		if (file->buffer_offset + size >= end_offset)
			return 1;
	}

	if (MAIL_TRANSACTION_LOG_FILE_IN_MEMORY(file)) {
		if (start_offset < file->buffer_offset) {
			/* we had moved the log to memory but failed to read
			   the beginning of the log file */
			mail_index_set_error(index,
				"%s: Beginning of the log isn't available",
				file->filepath);
			return 0;
		}
		return log_file_map_check_offsets(file, start_offset,
						  end_offset);
	}

	if (!index->mmap_disable) {
		/* we are going to mmap() this file, but it's not necessarily
		   mmaped currently. */
		i_assert(file->buffer_offset == 0 || file->mmap_base == NULL);
		i_assert(file->mmap_size == 0 || file->mmap_base != NULL);

		if (fstat(file->fd, &st) < 0) {
			mail_index_file_set_syscall_error(index, file->filepath,
							  "fstat()");
			return -1;
		}
		file->last_size = st.st_size;

		if ((uoff_t)st.st_size < file->sync_offset) {
			mail_transaction_log_file_set_corrupted(file,
				"file size shrank");
			return 0;
		}

		if ((uoff_t)st.st_size == file->mmap_size) {
			/* we already have the whole file mmaped */
			if ((ret = mail_transaction_log_file_sync(file)) < 0)
				return 0;
			if (ret > 0) {
				return log_file_map_check_offsets(file,
								  start_offset,
								  end_offset);
			}
			/* size changed, re-mmap */
		}
	}

	if (index->mmap_disable) {
		mail_transaction_log_file_munmap(file);

		ret = mail_transaction_log_file_read(file, start_offset);
		if (ret <= 0)
			return ret;
	} else {
		do {
			mail_transaction_log_file_munmap(file);

			if (mail_transaction_log_file_mmap(file) < 0)
				return -1;
			if ((ret = mail_transaction_log_file_sync(file)) < 0)
				return 0;
		} while (ret == 0);
	}

	return log_file_map_check_offsets(file, start_offset, end_offset);
}

void mail_transaction_log_file_move_to_memory(struct mail_transaction_log_file
					      *file)
{
	buffer_t *buf;

	if (MAIL_TRANSACTION_LOG_FILE_IN_MEMORY(file))
		return;

	if (file->mmap_base != NULL) {
		/* just copy to memory */
		i_assert(file->buffer_offset == 0);

		buf = buffer_create_dynamic(default_pool, file->mmap_size);
		buffer_append(buf, file->mmap_base, file->mmap_size);
		buffer_free(&file->buffer);
		file->buffer = buf;

		/* and lose the mmap */
		if (munmap(file->mmap_base, file->mmap_size) < 0) {
			mail_index_file_set_syscall_error(file->log->index,
							  file->filepath,
							  "munmap()");
		}
		file->mmap_base = NULL;
	} else if (file->buffer_offset != 0) {
		/* we don't have the full log in the memory. read it. */
		(void)mail_transaction_log_file_read(file, 0);
	}

	if (close(file->fd) < 0) {
		mail_index_file_set_syscall_error(file->log->index,
						  file->filepath, "close()");
	}
	file->fd = -1;
}
