/* Copyright (C) 2002 Timo Sirainen */

#include "lib.h"
#include "ioloop.h"
#include "hostpid.h"
#include "istream.h"
#include "ostream.h"
#include "str.h"
#include "write-full.h"
#include "istream-header-filter.h"
#include "ostream-crlf.h"
#include "message-parser.h"
#include "mbox-storage.h"
#include "mbox-file.h"
#include "mbox-from.h"
#include "mbox-lock.h"
#include "mbox-md5.h"
#include "mbox-sync-private.h"

#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <netdb.h>

struct mbox_save_context {
	struct mail_save_context ctx;

	struct mbox_mailbox *mbox;
	struct mail_index_transaction *trans;
	uoff_t append_offset, mail_offset;

	string_t *headers;
	size_t space_end_idx;
	uint32_t seq, next_uid;

	struct istream *input;
	struct ostream *output, *body_output;
	uoff_t extra_hdr_offset, eoh_offset, eoh_input_offset;
	char last_char;

	struct mbox_md5_context *mbox_md5_ctx;

	unsigned int synced:1;
	unsigned int failed:1;
};

static char my_hostdomain[256] = "";

static void write_error(struct mbox_save_context *ctx, int error)
{
	if (ENOSPACE(error)) {
		mail_storage_set_error(STORAGE(ctx->mbox->storage),
				       "Not enough disk space");
	} else {
		errno = error;
		mbox_set_syscall_error(ctx->mbox, "write()");
	}
}

static int mbox_seek_to_end(struct mbox_save_context *ctx, uoff_t *offset)
{
	struct stat st;
	char ch;
	int fd;

	if (ctx->mbox->mbox_writeonly) {
		*offset = 0;
		return 0;
	}

	fd = ctx->mbox->mbox_fd;
	if (fstat(fd, &st) < 0)
                return mbox_set_syscall_error(ctx->mbox, "fstat()");

	*offset = (uoff_t)st.st_size;
	if (st.st_size == 0)
		return 0;

	if (lseek(fd, st.st_size-1, SEEK_SET) < 0)
                return mbox_set_syscall_error(ctx->mbox, "lseek()");

	if (read(fd, &ch, 1) != 1)
		return mbox_set_syscall_error(ctx->mbox, "read()");

	if (ch != '\n') {
		if (write_full(fd, "\n", 1) < 0) {
			write_error(ctx, errno);
			return -1;
		}
		*offset += 1;
	}

	return 0;
}

static int mbox_append_lf(struct mbox_save_context *ctx)
{
	if (o_stream_send(ctx->output, "\n", 1) < 0) {
		write_error(ctx, ctx->output->stream_errno);
		return -1;
	}

	return 0;
}

static int write_from_line(struct mbox_save_context *ctx, time_t received_date,
			   const char *from_envelope)
{
	const char *line, *name;
	int ret;

	if (*my_hostdomain == '\0') {
		struct hostent *hent;

		hent = gethostbyname(my_hostname);

		name = hent != NULL ? hent->h_name : NULL;
		if (name == NULL) {
			/* failed, use just the hostname */
			name = my_hostname;
		}

		strocpy(my_hostdomain, name, sizeof(my_hostdomain));
	}

	t_push();
	if (from_envelope == NULL) {
		from_envelope =
			t_strconcat(INDEX_STORAGE(ctx->mbox->storage)->user,
				    "@", my_hostdomain, NULL);
	}

	/* save in local timezone, no matter what it was given with */
	line = mbox_from_create(from_envelope, received_date);

	if ((ret = o_stream_send_str(ctx->output, line)) < 0)
		write_error(ctx, ctx->output->stream_errno);
	t_pop();

	return ret;
}

static int mbox_write_content_length(struct mbox_save_context *ctx)
{
	uoff_t end_offset;
	const char *str;
	size_t len;
	int ret = 0;

	if (ctx->mbox->mbox_writeonly) {
		/* we can't seek, don't set Content-Length */
		return 0;
	}

	end_offset = ctx->output->offset;

	/* write Content-Length headers */
	t_push();
	str = t_strdup_printf("\nContent-Length: %s",
			      dec2str(end_offset - ctx->eoh_offset));
	len = strlen(str);

	if (o_stream_seek(ctx->output, ctx->extra_hdr_offset +
			  ctx->space_end_idx - len) < 0) {
		mbox_set_syscall_error(ctx->mbox, "o_stream_seek()");
		ret = -1;
	} else if (o_stream_send(ctx->output, str, len) < 0) {
		write_error(ctx, ctx->output->stream_errno);
		ret = -1;
	} else {
		if (o_stream_seek(ctx->output, end_offset) < 0) {
			mbox_set_syscall_error(ctx->mbox, "o_stream_seek()");
			ret = -1;
		}
	}

	t_pop();
	return ret;
}

static void mbox_save_init_sync(struct mbox_transaction_context *t)
{
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)t->ictx.ibox;
	struct mbox_save_context *ctx = t->save_ctx;
	const struct mail_index_header *hdr;
	struct mail_index_view *view;

	/* open a new view to get the header. this is required if we just
	   synced the mailbox so we can get updated next_uid. */
	view = mail_index_view_open(mbox->ibox.index);
	hdr = mail_index_get_header(view);

	ctx->next_uid = hdr->next_uid;
	ctx->synced = TRUE;
        t->mbox_modified = TRUE;

	mail_index_view_close(view);
}

static void status_flags_append(string_t *str, enum mail_flags flags,
				const struct mbox_flag_type *flags_list)
{
	int i;

	flags ^= MBOX_NONRECENT_KLUDGE;
	for (i = 0; flags_list[i].chr != 0; i++) {
		if ((flags & flags_list[i].flag) != 0)
			str_append_c(str, flags_list[i].chr);
	}
	flags ^= MBOX_NONRECENT_KLUDGE;
}

static void mbox_save_append_flag_headers(string_t *str, enum mail_flags flags)
{
	if ((flags & STATUS_FLAGS_MASK) != 0) {
		str_append(str, "Status: ");
		status_flags_append(str, flags, mbox_status_flags);
		str_append_c(str, '\n');
	}

	if ((flags & XSTATUS_FLAGS_MASK) != 0) {
		str_append(str, "X-Status: ");
		status_flags_append(str, flags, mbox_xstatus_flags);
		str_append_c(str, '\n');
	}
}

static void
mbox_save_append_keyword_headers(struct mbox_save_context *ctx,
				 struct mail_keywords *keywords)
{
	unsigned char space[MBOX_HEADER_PADDING+1 +
			    sizeof("Content-Length: \n")-1 + MAX_INT_STRLEN];
	const array_t *keyword_names_list;
	ARRAY_SET_TYPE(keyword_names_list, const char *);
	const char *const *keyword_names;
	unsigned int i, count, keyword_names_count;

	keyword_names_list = mail_index_get_keywords(ctx->mbox->ibox.index);
	keyword_names = array_get(keyword_names_list, &keyword_names_count);

	str_append(ctx->headers, "X-Keywords:");
	count = keywords == NULL ? 0 : keywords->count;
	for (i = 0; i < count; i++) {
		i_assert(keywords->idx[i] < keyword_names_count);

		str_append_c(ctx->headers, ' ');
		str_append(ctx->headers, keyword_names[keywords->idx[i]]);
	}

	memset(space, ' ', sizeof(space));
	str_append_n(ctx->headers, space, sizeof(space));
	ctx->space_end_idx = str_len(ctx->headers);
	str_append_c(ctx->headers, '\n');
}

static int
mbox_save_init_file(struct mbox_save_context *ctx,
		    struct mbox_transaction_context *t, int want_mail)
{
	struct mbox_mailbox *mbox = ctx->mbox;
	int ret;

	if (ctx->mbox->mbox_readonly || ctx->mbox->ibox.readonly) {
		mail_storage_set_error(STORAGE(ctx->mbox->storage),
				       "Read-only mbox");
		return -1;
	}

	if (ctx->append_offset == (uoff_t)-1) {
		/* first appended mail in this transaction */
		if (mbox->mbox_lock_type != F_WRLCK) {
			if (mbox_lock(mbox, F_WRLCK, &t->mbox_lock_id) <= 0)
				return -1;
		}

		if (mbox->mbox_fd == -1) {
			if (mbox_file_open(mbox) < 0)
				return -1;
		}

		if (!want_mail) {
			/* assign UIDs only if mbox doesn't require syncing */
			ret = mbox_sync_has_changed(mbox, TRUE);
			if (ret < 0)
				return -1;
			if (ret == 0)
				mbox_save_init_sync(t);
		}

		if (mbox_seek_to_end(ctx, &ctx->append_offset) < 0)
			return -1;

		ctx->output = o_stream_create_file(mbox->mbox_fd, default_pool,
						   0, FALSE);
	}

	if (!ctx->synced && want_mail) {
		/* we'll need to assign UID for the mail immediately. */
		if (mbox_sync(mbox, 0) < 0)
			return -1;
		mbox_save_init_sync(t);
	}

	return 0;
}

static void save_header_callback(struct message_header_line *hdr,
				 int *matched, void *context)
{
	struct mbox_save_context *ctx = context;

	if (!*matched && ctx->mbox_md5_ctx && hdr != NULL)
		mbox_md5_continue(ctx->mbox_md5_ctx, hdr);

	if ((hdr == NULL && ctx->eoh_input_offset == (uoff_t)-1) ||
	    (hdr != NULL && hdr->eoh))
		ctx->eoh_input_offset = ctx->input->v_offset;
}

struct mail_save_context *
mbox_save_init(struct mailbox_transaction_context *_t,
	       enum mail_flags flags, struct mail_keywords *keywords,
	       time_t received_date, int timezone_offset __attr_unused__,
	       const char *from_envelope, struct istream *input, int want_mail)
{
	struct mbox_transaction_context *t =
		(struct mbox_transaction_context *)_t;
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)t->ictx.ibox;
	struct mbox_save_context *ctx = t->save_ctx;
	enum mail_flags save_flags;
	uint64_t offset;

	i_assert((t->ictx.flags & MAILBOX_TRANSACTION_FLAG_EXTERNAL) != 0);

	/* FIXME: we could write timezone_offset to From-line.. */
	if (received_date == (time_t)-1)
		received_date = ioloop_time;

	if (ctx == NULL) {
		ctx = t->save_ctx = i_new(struct mbox_save_context, 1);
		ctx->ctx.transaction = &t->ictx.mailbox_ctx;
		ctx->mbox = mbox;
		ctx->trans = t->ictx.trans;
		ctx->append_offset = (uoff_t)-1;
		ctx->headers = str_new(default_pool, 512);
		ctx->mail_offset = (uoff_t)-1;
	}

	ctx->failed = FALSE;
	ctx->seq = 0;

	if (mbox_save_init_file(ctx, t, want_mail) < 0) {
		ctx->failed = TRUE;
		return &ctx->ctx;
	}

	save_flags = (flags & ~MAIL_RECENT) | MAIL_RECENT;
	str_truncate(ctx->headers, 0);
	if (ctx->synced) {
		str_printfa(ctx->headers, "X-UID: %u\n", ctx->next_uid);
		if (!mbox->ibox.keep_recent)
			save_flags &= ~MAIL_RECENT;

		// FIXME: set keywords
		mail_index_append(ctx->trans, ctx->next_uid, &ctx->seq);
		mail_index_update_flags(ctx->trans, ctx->seq, MODIFY_REPLACE,
					save_flags);

		offset = ctx->output->offset == 0 ? 0 :
			ctx->output->offset - 1;
		mail_index_update_ext(ctx->trans, ctx->seq,
				      mbox->mbox_ext_idx, &offset, NULL);
		ctx->next_uid++;
	}
	mbox_save_append_flag_headers(ctx->headers, save_flags);
	mbox_save_append_keyword_headers(ctx, keywords);
	str_append_c(ctx->headers, '\n');

	i_assert(mbox->mbox_lock_type == F_WRLCK);

	ctx->mail_offset = ctx->output->offset;
	ctx->eoh_input_offset = (uoff_t)-1;
	ctx->eoh_offset = (uoff_t)-1;
	ctx->last_char = '\n';

	if (write_from_line(ctx, received_date, from_envelope) < 0)
		ctx->failed = TRUE;
	else {
		ctx->input =
			i_stream_create_header_filter(input,
						      HEADER_FILTER_EXCLUDE |
                                                      HEADER_FILTER_NO_CR,
						      mbox_hide_headers,
						      mbox_hide_headers_count,
						      save_header_callback,
						      ctx);
		ctx->body_output =
			(STORAGE(mbox->storage)->flags &
			 MAIL_STORAGE_FLAG_SAVE_CRLF) != 0 ?
			o_stream_create_crlf(default_pool, ctx->output) :
			o_stream_create_lf(default_pool, ctx->output);
		if (ctx->mbox->mbox_save_md5 && ctx->synced)
			ctx->mbox_md5_ctx = mbox_md5_init();
	}

	return &ctx->ctx;
}

int mbox_save_continue(struct mail_save_context *_ctx)
{
	struct mbox_save_context *ctx = (struct mbox_save_context *)_ctx;
	const unsigned char *data;
	size_t size;
	ssize_t ret;

	if (ctx->failed)
		return -1;

	if (ctx->eoh_offset != (uoff_t)-1) {
		/* writing body */
		if (o_stream_send_istream(ctx->body_output, ctx->input) < 0) {
			ctx->failed = TRUE;
			return -1;
		}
		return 0;
	}

	while ((ret = i_stream_read(ctx->input)) != -1) {
		if (ret == 0)
			return 0;

		data = i_stream_get_data(ctx->input, &size);
		if (ctx->eoh_input_offset != (uoff_t)-1 &&
		    ctx->input->v_offset + size >= ctx->eoh_input_offset) {
			/* found end of headers. write the rest of them. */
			size = ctx->eoh_input_offset - ctx->input->v_offset;
			if (o_stream_send(ctx->output, data, size) < 0) {
				ctx->failed = TRUE;
				return -1;
			}
			if (size > 0)
				ctx->last_char = data[size-1];
			i_stream_skip(ctx->input, size + 1);
			break;
		}

		if (o_stream_send(ctx->output, data, size) < 0) {
			ctx->failed = TRUE;
			return -1;
		}
		ctx->last_char = data[size-1];
		i_stream_skip(ctx->input, size);
	}

	if (ctx->last_char != '\n') {
		if (o_stream_send(ctx->output, "\n", 1) < 0) {
			ctx->failed = TRUE;
			return -1;
		}
	}

	if (ctx->mbox_md5_ctx) {
		unsigned char hdr_md5_sum[16];

		mbox_md5_finish(ctx->mbox_md5_ctx, hdr_md5_sum);
		mail_index_update_ext(ctx->trans, ctx->seq,
				      ctx->mbox->ibox.md5hdr_ext_idx,
				      hdr_md5_sum, NULL);
	}

	/* append our own headers and ending empty line */
	ctx->extra_hdr_offset = ctx->output->offset;
	if (o_stream_send(ctx->output, str_data(ctx->headers),
			  str_len(ctx->headers)) < 0) {
		ctx->failed = TRUE;
		return -1;
	}
	ctx->eoh_offset = ctx->output->offset;

	/* write body */
	(void)i_stream_get_data(ctx->input, &size);
	return ctx->input->eof && size == 0 ? 0 : mbox_save_continue(_ctx);
}

int mbox_save_finish(struct mail_save_context *_ctx, struct mail *dest_mail)
{
	struct mbox_save_context *ctx = (struct mbox_save_context *)_ctx;

	if (!ctx->failed) {
		if (mbox_write_content_length(ctx) < 0 ||
		    mbox_append_lf(ctx) < 0)
			ctx->failed = TRUE;
	}

	if (ctx->input != NULL) {
		i_stream_unref(ctx->input);
		ctx->input = NULL;
	}
	if (ctx->body_output != NULL) {
		o_stream_unref(ctx->body_output);
		ctx->body_output = NULL;
	}

	if (ctx->failed && ctx->mail_offset != (uoff_t)-1) {
		/* saving this mail failed - truncate back to beginning of it */
		if (ftruncate(ctx->mbox->mbox_fd, (off_t)ctx->mail_offset) < 0)
			mbox_set_syscall_error(ctx->mbox, "ftruncate()");
		ctx->mail_offset = (uoff_t)-1;
	}

	if (ctx->failed) {
		errno = ctx->output->stream_errno;
		if (ENOSPACE(errno)) {
			mail_storage_set_error(STORAGE(ctx->mbox->storage),
					       "Not enough disk space");
		} else if (errno != 0) {
			mail_storage_set_critical(STORAGE(ctx->mbox->storage),
				"write(%s) failed: %m", ctx->mbox->path);
		}
		return -1;
	}

	if (dest_mail != NULL) {
		i_assert(ctx->seq != 0);

		if (mail_set_seq(dest_mail, ctx->seq) < 0)
			return -1;
	}

	return 0;
}

void mbox_save_cancel(struct mail_save_context *_ctx)
{
	struct mbox_save_context *ctx = (struct mbox_save_context *)_ctx;

	ctx->failed = TRUE;
	(void)mbox_save_finish(_ctx, NULL);
}

static void mbox_transaction_save_deinit(struct mbox_save_context *ctx)
{
	i_assert(ctx->body_output == NULL);

	if (ctx->output != NULL)
		o_stream_unref(ctx->output);
	str_free(ctx->headers);
	i_free(ctx);
}

int mbox_transaction_save_commit(struct mbox_save_context *ctx)
{
	int ret = 0;

	if (ctx->synced) {
		mail_index_update_header(ctx->trans,
			offsetof(struct mail_index_header, next_uid),
			&ctx->next_uid, sizeof(ctx->next_uid), FALSE);
	}

	if (!ctx->synced && ctx->mbox->mbox_fd != -1 &&
	    !ctx->mbox->mbox_writeonly) {
		if (fdatasync(ctx->mbox->mbox_fd) < 0) {
			mbox_set_syscall_error(ctx->mbox, "fdatasync()");
			ret = -1;
		}
	}

	mbox_transaction_save_deinit(ctx);
	return ret;
}

void mbox_transaction_save_rollback(struct mbox_save_context *ctx)
{
	struct mbox_mailbox *mbox = ctx->mbox;

	if (ctx->append_offset != (uoff_t)-1 && mbox->mbox_fd != -1) {
		i_assert(mbox->mbox_lock_type == F_WRLCK);

		/* failed, truncate file back to original size.
		   output stream needs to be flushed before truncating
		   so unref() won't write anything. */
		o_stream_flush(ctx->output);

		if (ftruncate(mbox->mbox_fd, (off_t)ctx->append_offset) < 0)
			mbox_set_syscall_error(mbox, "ftruncate()");
	}

	mbox_transaction_save_deinit(ctx);
}
