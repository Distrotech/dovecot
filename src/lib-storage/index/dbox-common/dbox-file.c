/* Copyright (c) 2007-2010 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "hex-dec.h"
#include "hex-binary.h"
#include "hostpid.h"
#include "istream.h"
#include "ostream.h"
#include "file-lock.h"
#include "mkdir-parents.h"
#include "fdatasync-path.h"
#include "eacces-error.h"
#include "str.h"
#include "dbox-storage.h"
#include "dbox-file.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>

#define DBOX_READ_BLOCK_SIZE 4096

const char *dbox_generate_tmp_filename(void)
{
	static unsigned int create_count = 0;

	return t_strdup_printf("temp.%lu.P%sQ%uM%u.%s",
			       (unsigned long)ioloop_timeval.tv_sec, my_pid,
			       create_count++,
			       (unsigned int)ioloop_timeval.tv_usec,
			       my_hostname);
}

void dbox_file_set_syscall_error(struct dbox_file *file, const char *function)
{
	mail_storage_set_critical(&file->storage->storage,
				  "%s failed for file %s: %m",
				  function, file->cur_path);
}

void dbox_file_set_corrupted(struct dbox_file *file, const char *reason, ...)
{
	va_list args;

	file->storage->files_corrupted = TRUE;

	va_start(args, reason);
	mail_storage_set_critical(&file->storage->storage,
		"Corrupted dbox file %s (around offset=%"PRIuUOFF_T"): %s",
		file->cur_path, file->input == NULL ? 0 : file->input->v_offset,
		t_strdup_vprintf(reason, args));
	va_end(args);
}

void dbox_file_init(struct dbox_file *file)
{
	file->refcount = 1;
	file->fd = -1;
	file->cur_offset = (uoff_t)-1;
	file->cur_path = file->primary_path;
}

void dbox_file_free(struct dbox_file *file)
{
	i_assert(file->refcount == 0);

	if (file->metadata_pool != NULL)
		pool_unref(&file->metadata_pool);
	dbox_file_close(file);
	i_free(file->primary_path);
	i_free(file->alt_path);
	i_free(file);
}

void dbox_file_unref(struct dbox_file **_file)
{
	struct dbox_file *file = *_file;

	*_file = NULL;

	i_assert(file->refcount > 0);
	if (--file->refcount == 0)
		file->storage->v.file_unrefed(file);
}

static int dbox_file_parse_header(struct dbox_file *file, const char *line)
{
	const char *const *tmp, *value;
	unsigned int pos;
	enum dbox_header_key key;

	file->file_version = *line - '0';
	if (!i_isdigit(line[0]) || line[1] != ' ' ||
	    (file->file_version != 1 && file->file_version != DBOX_VERSION)) {
		dbox_file_set_corrupted(file, "Invalid dbox version");
		return -1;
	}
	line += 2;
	pos = 2;

	file->msg_header_size = 0;

	for (tmp = t_strsplit(line, " "); *tmp != NULL; tmp++) {
		key = **tmp;
		value = *tmp + 1;

		switch (key) {
		case DBOX_HEADER_OLDV1_APPEND_OFFSET:
			break;
		case DBOX_HEADER_MSG_HEADER_SIZE:
			file->msg_header_size = strtoul(value, NULL, 16);
			break;
		case DBOX_HEADER_CREATE_STAMP:
			file->create_time = strtoul(value, NULL, 16);
			break;
		}
		pos += strlen(value) + 2;
	}

	if (file->msg_header_size == 0) {
		dbox_file_set_corrupted(file, "Missing message header size");
		return -1;
	}
	return 0;
}

static int dbox_file_read_header(struct dbox_file *file)
{
	const char *line;
	unsigned int hdr_size;
	int ret;

	i_stream_seek(file->input, 0);
	line = i_stream_read_next_line(file->input);
	if (line == NULL) {
		if (file->input->stream_errno == 0) {
			dbox_file_set_corrupted(file,
				"EOF while reading file header");
			return 0;
		}

		dbox_file_set_syscall_error(file, "read()");
		return -1;
	}
	hdr_size = file->input->v_offset;
	T_BEGIN {
		ret = dbox_file_parse_header(file, line) < 0 ? 0 : 1;
	} T_END;
	if (ret > 0)
		file->file_header_size = hdr_size;
	return ret;
}

static int dbox_file_open_fd(struct dbox_file *file)
{
	const char *path;
	bool alt = FALSE;

	/* try the primary path first */
	path = file->primary_path;
	while ((file->fd = open(path, O_RDWR)) == -1) {
		if (errno != ENOENT) {
			mail_storage_set_critical(&file->storage->storage,
						  "open(%s) failed: %m", path);
			return -1;
		}

		if (file->alt_path == NULL || alt) {
			/* not found */
			return 0;
		}

		/* try the alternative path */
		path = file->alt_path;
		alt = TRUE;
	}
	file->cur_path = path;
	return 1;
}

int dbox_file_open(struct dbox_file *file, bool *deleted_r)
{
	int ret;

	*deleted_r = FALSE;
	if (file->input != NULL)
		return 1;

	if (file->fd == -1) {
		T_BEGIN {
			ret = dbox_file_open_fd(file);
		} T_END;
		if (ret <= 0) {
			if (ret < 0)
				return -1;
			*deleted_r = TRUE;
			return 1;
		}
	}

	file->input = i_stream_create_fd(file->fd, 0, FALSE);
	i_stream_set_init_buffer_size(file->input, DBOX_READ_BLOCK_SIZE);
	return dbox_file_read_header(file);
}

int dbox_file_header_write(struct dbox_file *file, struct ostream *output)
{
	string_t *hdr;

	hdr = t_str_new(128);
	str_printfa(hdr, "%u %c%x %c%x\n", DBOX_VERSION,
		    DBOX_HEADER_MSG_HEADER_SIZE,
		    (unsigned int)sizeof(struct dbox_message_header),
		    DBOX_HEADER_CREATE_STAMP, (unsigned int)ioloop_time);

	file->file_version = DBOX_VERSION;
	file->file_header_size = str_len(hdr);
	file->msg_header_size = sizeof(struct dbox_message_header);
	return o_stream_send(output, str_data(hdr), str_len(hdr));
}

void dbox_file_close(struct dbox_file *file)
{
	dbox_file_unlock(file);
	if (file->input != NULL)
		i_stream_unref(&file->input);
	if (file->fd != -1) {
		if (close(file->fd) < 0)
			dbox_file_set_syscall_error(file, "close()");
		file->fd = -1;
	}
	file->cur_offset = (uoff_t)-1;
}

int dbox_file_try_lock(struct dbox_file *file)
{
	int ret;

	i_assert(file->fd != -1);

	ret = file_try_lock(file->fd, file->cur_path, F_WRLCK,
			    FILE_LOCK_METHOD_FCNTL, &file->lock);
	if (ret < 0) {
		mail_storage_set_critical(&file->storage->storage,
			"file_try_lock(%s) failed: %m", file->cur_path);
	}
	return ret;
}

void dbox_file_unlock(struct dbox_file *file)
{
	i_assert(!file->appending);

	if (file->lock != NULL)
		file_unlock(&file->lock);
	if (file->input != NULL)
		i_stream_sync(file->input);
}

int dbox_file_read_mail_header(struct dbox_file *file, uoff_t *physical_size_r)
{
	struct dbox_message_header hdr;
	const unsigned char *data;
	size_t size;
	int ret;

	ret = i_stream_read_data(file->input, &data, &size,
				 file->msg_header_size - 1);
	if (ret <= 0) {
		if (file->input->stream_errno == 0) {
			/* EOF, broken offset or file truncated */
			dbox_file_set_corrupted(file, "EOF reading msg header "
						"(got %"PRIuSIZE_T"/%u bytes)",
						size, file->msg_header_size);
			return 0;
		}
		dbox_file_set_syscall_error(file, "read()");
		return -1;
	}
	memcpy(&hdr, data, I_MIN(sizeof(hdr), file->msg_header_size));
	if (memcmp(hdr.magic_pre, DBOX_MAGIC_PRE, sizeof(hdr.magic_pre)) != 0) {
		/* probably broken offset */
		dbox_file_set_corrupted(file, "msg header has bad magic value");
		return 0;
	}

	if (data[file->msg_header_size-1] != '\n') {
		dbox_file_set_corrupted(file, "msg header doesn't end with LF");
		return 0;
	}

	*physical_size_r = hex2dec(hdr.message_size_hex,
				   sizeof(hdr.message_size_hex));
	return 1;
}

int dbox_file_get_mail_stream(struct dbox_file *file, uoff_t offset,
			      uoff_t *physical_size_r,
			      struct istream **stream_r)
{
	uoff_t size;
	int ret;

	i_assert(file->input != NULL);

	if (offset == 0)
		offset = file->file_header_size;

	if (offset != file->cur_offset) {
		i_stream_seek(file->input, offset);
		ret = dbox_file_read_mail_header(file, &size);
		if (ret <= 0)
			return ret;
		file->cur_offset = offset;
		file->cur_physical_size = size;
	}
	i_stream_seek(file->input, offset + file->msg_header_size);
	if (stream_r != NULL) {
		*stream_r = i_stream_create_limit(file->input,
						  file->cur_physical_size);
	}
	*physical_size_r = file->cur_physical_size;
	return 1;
}

static int
dbox_file_seek_next_at_metadata(struct dbox_file *file, uoff_t *offset)
{
	const char *line;
	int ret;

	i_stream_seek(file->input, *offset);
	if ((ret = dbox_file_metadata_skip_header(file)) <= 0)
		return ret;

	/* skip over the actual metadata */
	while ((line = i_stream_read_next_line(file->input)) != NULL) {
		if (*line == DBOX_METADATA_OLDV1_SPACE || *line == '\0') {
			/* end of metadata */
			break;
		}
	}
	*offset = file->input->v_offset;
	return 1;
}

void dbox_file_seek_rewind(struct dbox_file *file)
{
	file->cur_offset = (uoff_t)-1;
}

int dbox_file_seek_next(struct dbox_file *file, uoff_t *offset_r, bool *last_r)
{
	uoff_t offset, size;
	int ret;

	i_assert(file->input != NULL);

	if (file->cur_offset == (uoff_t)-1) {
		/* first mail. we may not have read the file at all yet,
		   so set the offset afterwards. */
		offset = 0;
	} else {
		offset = file->cur_offset + file->msg_header_size +
			file->cur_physical_size;
		if ((ret = dbox_file_seek_next_at_metadata(file, &offset)) <= 0) {
			*offset_r = file->cur_offset;
			return ret;
		}
	}
	*offset_r = offset;

	if (i_stream_is_eof(file->input)) {
		*last_r = TRUE;
		return 0;
	}
	*last_r = FALSE;

	ret = dbox_file_get_mail_stream(file, offset, &size, NULL);
	if (*offset_r == 0)
		*offset_r = file->file_header_size;
	return ret;
}

struct dbox_file_append_context *dbox_file_append_init(struct dbox_file *file)
{
	struct dbox_file_append_context *ctx;

	i_assert(!file->appending);

	file->appending = TRUE;

	ctx = i_new(struct dbox_file_append_context, 1);
	ctx->file = file;
	if (file->fd != -1) {
		ctx->output = o_stream_create_fd_file(file->fd, 0, FALSE);
		o_stream_cork(ctx->output);
	}
	return ctx;
}

int dbox_file_append_commit(struct dbox_file_append_context **_ctx)
{
	struct dbox_file_append_context *ctx = *_ctx;
	int ret;

	i_assert(ctx->file->appending);

	*_ctx = NULL;

	ret = dbox_file_append_flush(ctx);
	o_stream_unref(&ctx->output);
	ctx->file->appending = FALSE;
	i_free(ctx);
	return 0;
}

void dbox_file_append_rollback(struct dbox_file_append_context **_ctx)
{
	struct dbox_file_append_context *ctx = *_ctx;
	struct dbox_file *file = ctx->file;
	bool close_file = FALSE;

	i_assert(ctx->file->appending);

	*_ctx = NULL;
	if (ctx->first_append_offset == 0) {
		/* nothing changed */
	} else if (ctx->first_append_offset == file->file_header_size) {
		/* rollbacking everything */
		if (unlink(file->cur_path) < 0)
			dbox_file_set_syscall_error(file, "unlink()");
		close_file = TRUE;
	} else {
		/* truncating only some mails */
		o_stream_close(ctx->output);
		if (ftruncate(file->fd, ctx->first_append_offset) < 0)
			dbox_file_set_syscall_error(file, "ftruncate()");
	}
	if (ctx->output != NULL)
		o_stream_unref(&ctx->output);
	i_free(ctx);

	if (close_file)
		dbox_file_close(file);
	file->appending = FALSE;
}

int dbox_file_append_flush(struct dbox_file_append_context *ctx)
{
	if (ctx->last_flush_offset == ctx->output->offset)
		return 0;

	if (o_stream_flush(ctx->output) < 0) {
		dbox_file_set_syscall_error(ctx->file, "write()");
		return -1;
	}

	if (!ctx->file->storage->storage.set->fsync_disable) {
		if (fdatasync(ctx->file->fd) < 0) {
			dbox_file_set_syscall_error(ctx->file, "fdatasync()");
			return -1;
		}
	}
	ctx->last_flush_offset = ctx->output->offset;
	return 0;
}

int dbox_file_get_append_stream(struct dbox_file_append_context *ctx,
				struct ostream **output_r)
{
	struct dbox_file *file = ctx->file;
	struct stat st;

	if (ctx->output == NULL) {
		/* file creation had failed */
		return -1;
	}

	if (file->file_version == 0) {
		/* newly created file, write the file header */
		if (dbox_file_header_write(file, ctx->output) < 0) {
			dbox_file_set_syscall_error(file, "write()");
			return -1;
		}
		*output_r = ctx->output;
		return 1;
	}

	/* file has existing mails */
	if (file->file_version != DBOX_VERSION ||
	    file->msg_header_size != sizeof(struct dbox_message_header)) {
		/* created by an incompatible version, can't append */
		return 0;
	}

	if (ctx->output->offset == 0) {
		/* first append to existing file. seek to eof first. */
		if (fstat(file->fd, &st) < 0) {
			dbox_file_set_syscall_error(file, "fstat()");
			return -1;
		}
		o_stream_seek(ctx->output, st.st_size);
	}
	*output_r = ctx->output;
	return 1;
}

int dbox_file_metadata_skip_header(struct dbox_file *file)
{
	struct dbox_metadata_header metadata_hdr;
	const unsigned char *data;
	size_t size;
	int ret;

	ret = i_stream_read_data(file->input, &data, &size,
				 sizeof(metadata_hdr) - 1);
	if (ret <= 0) {
		if (file->input->stream_errno == 0) {
			/* EOF, broken offset */
			dbox_file_set_corrupted(file,
				"Unexpected EOF while reading metadata header");
			return 0;
		}
		dbox_file_set_syscall_error(file, "read()");
		return -1;
	}
	memcpy(&metadata_hdr, data, sizeof(metadata_hdr));
	if (memcmp(metadata_hdr.magic_post, DBOX_MAGIC_POST,
		   sizeof(metadata_hdr.magic_post)) != 0) {
		/* probably broken offset */
		dbox_file_set_corrupted(file,
			"metadata header has bad magic value");
		return 0;
	}
	i_stream_skip(file->input, sizeof(metadata_hdr));
	return 1;
}

static int
dbox_file_metadata_read_at(struct dbox_file *file, uoff_t metadata_offset)
{
	const char *line;
	int ret;

	if (file->metadata_pool != NULL)
		p_clear(file->metadata_pool);
	else {
		file->metadata_pool =
			pool_alloconly_create("dbox metadata", 1024);
	}
	p_array_init(&file->metadata, file->metadata_pool, 16);

	i_stream_seek(file->input, metadata_offset);
	if ((ret = dbox_file_metadata_skip_header(file)) <= 0)
		return ret;

	ret = 0;
	while ((line = i_stream_read_next_line(file->input)) != NULL) {
		if (*line == DBOX_METADATA_OLDV1_SPACE || *line == '\0') {
			/* end of metadata */
			ret = 1;
			break;
		}
		line = p_strdup(file->metadata_pool, line);
		array_append(&file->metadata, &line, 1);
	}
	if (ret == 0)
		dbox_file_set_corrupted(file, "missing end-of-metadata line");
	return ret;
}

int dbox_file_metadata_read(struct dbox_file *file)
{
	uoff_t metadata_offset;
	int ret;

	i_assert(file->cur_offset != (uoff_t)-1);

	if (file->metadata_read_offset == file->cur_offset)
		return 1;

	metadata_offset = file->cur_offset + file->msg_header_size +
		file->cur_physical_size;
	ret = dbox_file_metadata_read_at(file, metadata_offset);
	if (ret <= 0)
		return ret;

	file->metadata_read_offset = file->cur_offset;
	return 1;
}

const char *dbox_file_metadata_get(struct dbox_file *file,
				   enum dbox_metadata_key key)
{
	const char *const *metadata;
	unsigned int i, count;

	metadata = array_get(&file->metadata, &count);
	for (i = 0; i < count; i++) {
		if (*metadata[i] == (char)key)
			return metadata[i] + 1;
	}
	return NULL;
}

int dbox_file_move(struct dbox_file *file, bool alt_path)
{
	struct ostream *output;
	const char *dest_dir, *temp_path, *dest_path, *p;
	struct stat st;
	bool deleted;
	int out_fd, ret = 0;

	i_assert(file->input != NULL);
	i_assert(file->lock != NULL);

	if (dbox_file_is_in_alt(file) == alt_path)
		return 0;

	if (stat(file->cur_path, &st) < 0 && errno == ENOENT) {
		/* already expunged/moved by another session */
		dbox_file_unlock(file);
		return 0;
	}

	dest_path = !alt_path ? file->primary_path : file->alt_path;
	p = strrchr(dest_path, '/');
	i_assert(p != NULL);
	dest_dir = t_strdup_until(dest_path, p);
	temp_path = t_strdup_printf("%s/%s", dest_dir,
				    dbox_generate_tmp_filename());

	/* first copy the file. make sure to catch every possible error
	   since we really don't want to break the file. */
	out_fd = file->storage->v.file_create_fd(file, temp_path, TRUE);
	if (out_fd == -1)
		return -1;

	output = o_stream_create_fd_file(out_fd, 0, FALSE);
	i_stream_seek(file->input, 0);
	while ((ret = o_stream_send_istream(output, file->input)) > 0) ;
	if (ret == 0)
		ret = o_stream_flush(output);
	if (output->stream_errno != 0) {
		errno = output->stream_errno;
		mail_storage_set_critical(&file->storage->storage,
					  "write(%s) failed: %m", temp_path);
		ret = -1;
	} else if (file->input->stream_errno != 0) {
		errno = file->input->stream_errno;
		dbox_file_set_syscall_error(file, "ftruncate()");
		ret = -1;
	} else if (ret < 0) {
		mail_storage_set_critical(&file->storage->storage,
			"o_stream_send_istream(%s, %s) "
			"failed with unknown error",
			temp_path, file->cur_path);
	}
	o_stream_unref(&output);

	if (!file->storage->storage.set->fsync_disable && ret == 0) {
		if (fsync(out_fd) < 0) {
			mail_storage_set_critical(&file->storage->storage,
				"fsync(%s) failed: %m", temp_path);
			ret = -1;
		}
	}
	if (close(out_fd) < 0) {
		mail_storage_set_critical(&file->storage->storage,
			"close(%s) failed: %m", temp_path);
		ret = -1;
	}
	if (ret < 0) {
		(void)unlink(temp_path);
		return -1;
	}

	/* the temp file was successfully written. rename it now to the
	   destination file. the destination shouldn't exist, but if it does
	   its contents should be the same (except for maybe older metadata) */
	if (rename(temp_path, dest_path) < 0) {
		mail_storage_set_critical(&file->storage->storage,
			"rename(%s, %s) failed: %m", temp_path, dest_path);
		(void)unlink(temp_path);
		return -1;
	}
	if (!file->storage->storage.set->fsync_disable) {
		if (fdatasync_path(dest_dir) < 0) {
			mail_storage_set_critical(&file->storage->storage,
				"fdatasync(%s) failed: %m", dest_dir);
			(void)unlink(dest_path);
			return -1;
		}
	}
	if (unlink(file->cur_path) < 0) {
		dbox_file_set_syscall_error(file, "unlink()");
		if (errno == EACCES) {
			/* configuration problem? revert the write */
			(void)unlink(dest_path);
		}
		/* who knows what happened to the file. keep both just to be
		   sure both won't get deleted. */
		return -1;
	}

	/* file was successfully moved - reopen it */
	dbox_file_close(file);
	if (dbox_file_open(file, &deleted) <= 0) {
		mail_storage_set_critical(&file->storage->storage,
			"dbox_file_move(%s): reopening file failed", dest_path);
		return -1;
	}
	return 0;
}

void dbox_msg_header_fill(struct dbox_message_header *dbox_msg_hdr,
			  uoff_t message_size)
{
	memset(dbox_msg_hdr, ' ', sizeof(*dbox_msg_hdr));
	memcpy(dbox_msg_hdr->magic_pre, DBOX_MAGIC_PRE,
	       sizeof(dbox_msg_hdr->magic_pre));
	dbox_msg_hdr->type = DBOX_MESSAGE_TYPE_NORMAL;
	dec2hex(dbox_msg_hdr->message_size_hex, message_size,
		sizeof(dbox_msg_hdr->message_size_hex));
	dbox_msg_hdr->save_lf = '\n';
}

int dbox_file_unlink(struct dbox_file *file)
{
	const char *path;
	bool alt = FALSE;

	path = file->primary_path;
	while (unlink(path) < 0) {
		if (errno != ENOENT) {
			mail_storage_set_critical(&file->storage->storage,
				"unlink(%s) failed: %m", path);
			return -1;
		}
		if (file->alt_path == NULL || alt) {
			/* not found */
			i_warning("dbox: File unexpectedly lost: %s",
				  file->primary_path);
			return 0;
		}

		/* try the alternative path */
		path = file->alt_path;
		alt = TRUE;
	}
	return 1;
}
