/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "write-full.h"
#include "mail-index.h"
#include "mail-index-util.h"
#include "mail-cache.h"

#include <stdio.h>
#include <unistd.h>

int mail_index_truncate(struct mail_index *index)
{
	uoff_t empty_space, truncate_threshold;

	i_assert(index->lock_type == MAIL_LOCK_EXCLUSIVE);

	if (index->mmap_full_length <= INDEX_FILE_MIN_SIZE(index) ||
	    index->anon_mmap)
		return TRUE;

	/* really truncate the file only when it's almost empty */
	empty_space = index->mmap_full_length - index->mmap_used_length;
	truncate_threshold =
		index->mmap_full_length / 100 * INDEX_TRUNCATE_PERCENTAGE;

	if (empty_space > truncate_threshold) {
		index->mmap_full_length = index->mmap_used_length +
			(empty_space * INDEX_TRUNCATE_KEEP_PERCENTAGE / 100);

		/* keep the size record-aligned */
		index->mmap_full_length -= (index->mmap_full_length -
					    index->header_size) %
			sizeof(struct mail_index_record);

		if (index->mmap_full_length < INDEX_FILE_MIN_SIZE(index))
                        index->mmap_full_length = INDEX_FILE_MIN_SIZE(index);

		if (ftruncate(index->fd, (off_t)index->mmap_full_length) < 0)
			return index_set_syscall_error(index, "ftruncate()");

		index->header->sync_id++;
	}

	return TRUE;
}

#if 0
static int mail_index_copy_data(struct mail_index *index,
				int fd, const char *path)
{
	struct mail_index_data_header data_hdr;
	struct mail_index_data_record_header *rec_hdr;
	struct mail_index_record *rec;
	unsigned char *mmap_data;
	size_t mmap_data_size;
	uoff_t offset;

	mmap_data = mail_index_data_get_mmaped(index->data, &mmap_data_size);
	if (mmap_data == NULL)
		return FALSE;

	/* write data header */
	memset(&data_hdr, 0, sizeof(data_hdr));
	data_hdr.indexid = index->indexid;
	if (write_full(fd, &data_hdr, sizeof(data_hdr)) < 0) {
		index_file_set_syscall_error(index, path, "write_full()");
		return FALSE;
	}

	/* now we'll begin the actual moving. keep rebuild-flag on
	   while doing it. */
	index->header->flags |= MAIL_INDEX_FLAG_REBUILD;
	if (!mail_index_fmdatasync(index, index->header_size))
		return FALSE;

	offset = sizeof(data_hdr);
	rec = index->lookup(index, 1);
	while (rec != NULL) {
		if (rec->data_position + sizeof(*rec_hdr) > mmap_data_size) {
			index_set_corrupted(index,
				"data_position points outside file");
			return FALSE;
		}

		rec_hdr = (struct mail_index_data_record_header *)
			(mmap_data + rec->data_position);
		if (rec->data_position + rec_hdr->data_size > mmap_data_size) {
			index_set_corrupted(index,
				"data_size points outside file");
			return FALSE;
		}

		if (write_full(fd, mmap_data + rec->data_position,
			       rec_hdr->data_size) < 0) {
			index_file_set_syscall_error(index, path,
						     "write_full()");
			return FALSE;
		}

		rec->data_position = offset;
		offset += rec_hdr->data_size;

		rec = index->next(index, rec);
	}

	/* update header */
	data_hdr.used_file_size = offset;

	if (lseek(fd, 0, SEEK_SET) < 0)
		return index_file_set_syscall_error(index, path, "lseek()");

	if (write_full(fd, &data_hdr, sizeof(data_hdr)) < 0) {
		index_file_set_syscall_error(index, path, "write_full()");
		return FALSE;
	}

	return TRUE;
}

int mail_index_compress_data(struct mail_index *index)
{
	const char *temppath, *datapath;
	int fd, failed;

	if (index->anon_mmap)
		return TRUE;

	/* write the data into temporary file updating the offsets in index
	   while doing it. if we fail (especially if out of disk space/quota)
	   we'll simply fail and index is rebuilt later */
	if (!index->set_lock(index, MAIL_LOCK_EXCLUSIVE))
		return FALSE;

	fd = mail_index_create_temp_file(index, &temppath);
	if (fd == -1)
		return FALSE;

	failed = !mail_index_copy_data(index, fd, temppath);

	if (fdatasync(fd) < 0) {
		index_file_set_syscall_error(index, temppath, "fdatasync()");
		failed = TRUE;
	}

	if (close(fd) < 0) {
		index_file_set_syscall_error(index, temppath, "close()");
		failed = TRUE;
	}

	if (!failed) {
		/* now, rename the temp file to new data file. but before that
		   reset indexid to make sure that other processes know the
		   data file is closed. */
		(void)mail_index_data_mark_file_deleted(index->data);

		mail_index_data_free(index->data);

		datapath = t_strconcat(index->filepath, DATA_FILE_PREFIX, NULL);
		if (rename(temppath, datapath) < 0) {
			if (ENOSPACE(errno))
				index->nodiskspace = TRUE;

			index_set_error(index, "rename(%s, %s) failed: %m",
					temppath, datapath);
			failed = TRUE;
		}
	}

	if (failed) {
		if (unlink(temppath) < 0) {
			index_file_set_syscall_error(index, temppath,
						     "unlink()");
		}
		return FALSE;
	}

	/* make sure the whole file is synced before removing rebuild-flag */
	if (!mail_index_fmdatasync(index, index->mmap_used_length))
		return FALSE;

	index->header->flags &= ~(MAIL_INDEX_FLAG_COMPRESS_DATA |
				  MAIL_INDEX_FLAG_REBUILD);

	return mail_index_data_open(index);
}
#endif
