/* Copyright (c) 2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "aqueue.h"
#include "fd-set-nonblock.h"
#include "istream.h"
#include "istream-dot.h"
#include "ostream.h"
#include "str.h"
#include "strescape.h"
#include "imap-util.h"
#include "dsync-proxy.h"
#include "dsync-worker-private.h"

#include <stdlib.h>
#include <unistd.h>

#define OUTBUF_THROTTLE_SIZE (1024*64)

enum proxy_client_request_type {
	PROXY_CLIENT_REQUEST_TYPE_COPY,
	PROXY_CLIENT_REQUEST_TYPE_GET
};

struct proxy_client_request {
	enum proxy_client_request_type type;
	union {
		dsync_worker_msg_callback_t *get;
		dsync_worker_copy_callback_t *copy;
	} callback;
	void *context;
};

struct proxy_client_dsync_worker_mailbox_iter {
	struct dsync_worker_mailbox_iter iter;
	pool_t pool;
};

struct proxy_client_dsync_worker {
	struct dsync_worker worker;
	int fd_in, fd_out;
	struct io *io;
	struct istream *input;
	struct ostream *output;

	mailbox_guid_t selected_box_guid;

	struct istream *save_input;
	struct io *save_io;
	bool save_input_last_lf;

	pool_t msg_get_pool;
	struct dsync_msg_static_data msg_get_data;
	ARRAY_DEFINE(request_array, struct proxy_client_request);
	struct aqueue *request_queue;
};

extern struct dsync_worker_vfuncs proxy_client_dsync_worker;

static void proxy_client_worker_input(struct proxy_client_dsync_worker *worker);
static void proxy_client_send_stream(struct proxy_client_dsync_worker *worker);

static int
proxy_client_worker_read_line(struct proxy_client_dsync_worker *worker,
			      const char **line_r)
{
	if (worker->worker.failed)
		return -1;

	*line_r = i_stream_read_next_line(worker->input);
	if (*line_r == NULL) {
		if (worker->input->stream_errno != 0) {
			errno = worker->input->stream_errno;
			i_error("read() from worker server failed: %m");
			dsync_worker_set_failure(&worker->worker);
			return -1;
		}
		if (worker->input->eof) {
			i_error("worker server disconnected unexpectedly");
			dsync_worker_set_failure(&worker->worker);
			return -1;
		}
	}
	return *line_r != NULL ? 1 : 0;
}

static void
proxy_client_worker_msg_get_done(struct proxy_client_dsync_worker *worker)
{
	worker->msg_get_data.input = NULL;
	worker->io = io_add(worker->fd_in, IO_READ,
			    proxy_client_worker_input, worker);
}

static bool
proxy_client_worker_next_copy(const struct proxy_client_request *request,
			      const char *line)
{
	request->callback.copy(*line == '1', request->context);
	return TRUE;
}

static bool
proxy_client_worker_next_msg_get(struct proxy_client_dsync_worker *worker,
				 const struct proxy_client_request *request,
				 const char *line)
{
	enum dsync_msg_get_result result;
	const char *error;

	i_assert(worker->msg_get_data.input == NULL);
	p_clear(worker->msg_get_pool);
	switch (line[0]) {
	case '1':
		/* ok */
		if (dsync_proxy_msg_static_import(worker->msg_get_pool,
						  line, &worker->msg_get_data,
						  &error) < 0) {
			i_error("Invalid msg-get static input: %s", error);
			i_stream_close(worker->input);
			return FALSE;
		}
		worker->msg_get_data.input =
			i_stream_create_dot(worker->input, FALSE);
		i_stream_set_destroy_callback(worker->msg_get_data.input,
					      proxy_client_worker_msg_get_done,
					      worker);
		result = DSYNC_MSG_GET_RESULT_SUCCESS;
		break;
	case '0':
		/* expunged */
		result = DSYNC_MSG_GET_RESULT_EXPUNGED;
		break;
	default:
		/* failure */
		result = DSYNC_MSG_GET_RESULT_FAILED;
		break;
	}

	io_remove(&worker->io);
	request->callback.get(result, &worker->msg_get_data, request->context);
	return worker->io != NULL;
}

static bool
proxy_client_worker_next_reply(struct proxy_client_dsync_worker *worker,
			       const char *line)
{
	const struct proxy_client_request *requests;
	struct proxy_client_request request;
	bool ret = TRUE;

	requests = array_idx(&worker->request_array, 0);
	request = requests[aqueue_idx(worker->request_queue, 0)];
	aqueue_delete_tail(worker->request_queue);

	switch (request.type) {
	case PROXY_CLIENT_REQUEST_TYPE_COPY:
		ret = proxy_client_worker_next_copy(&request, line);
		break;
	case PROXY_CLIENT_REQUEST_TYPE_GET:
		ret = proxy_client_worker_next_msg_get(worker, &request, line);
		break;
	}
	return ret;
}

static void proxy_client_worker_input(struct proxy_client_dsync_worker *worker)
{
	const char *line;

	if (worker->worker.input_callback != NULL) {
		worker->worker.input_callback(worker->worker.input_context);
		return;
	}

	while (proxy_client_worker_read_line(worker, &line) > 0) {
		if (!proxy_client_worker_next_reply(worker, line))
			break;
	}
}

static int proxy_client_worker_output(struct proxy_client_dsync_worker *worker)
{
	int ret;

	if ((ret = o_stream_flush(worker->output)) < 0)
		return 1;

	if (worker->save_input != NULL) {
		/* proxy_client_worker_msg_save() hasn't finished yet. */
		o_stream_cork(worker->output);
		proxy_client_send_stream(worker);
		if (worker->save_input != NULL)
			return 1;
	}

	if (worker->worker.output_callback != NULL)
		worker->worker.output_callback(worker->worker.output_context);
	return ret;
}

struct dsync_worker *dsync_worker_init_proxy_client(int fd_in, int fd_out)
{
	struct proxy_client_dsync_worker *worker;

	worker = i_new(struct proxy_client_dsync_worker, 1);
	worker->worker.v = proxy_client_dsync_worker;
	worker->fd_in = fd_in;
	worker->fd_out = fd_out;
	worker->io = io_add(fd_in, IO_READ, proxy_client_worker_input, worker);
	worker->input = i_stream_create_fd(fd_in, (size_t)-1, FALSE);
	worker->output = o_stream_create_fd(fd_out, (size_t)-1, FALSE);
	/* we'll keep the output corked until flush is needed */
	o_stream_cork(worker->output);
	o_stream_set_flush_callback(worker->output, proxy_client_worker_output,
				    worker);
	fd_set_nonblock(fd_in, TRUE);
	fd_set_nonblock(fd_out, TRUE);

	worker->msg_get_pool = pool_alloconly_create("dsync proxy msg", 128);
	i_array_init(&worker->request_array, 64);
	worker->request_queue = aqueue_init(&worker->request_array.arr);

	return &worker->worker;
}

static void proxy_client_worker_deinit(struct dsync_worker *_worker)
{
	struct proxy_client_dsync_worker *worker =
		(struct proxy_client_dsync_worker *)_worker;

	io_remove(&worker->io);
	i_stream_destroy(&worker->input);
	o_stream_destroy(&worker->output);
	if (close(worker->fd_in) < 0)
		i_error("close(worker input) failed: %m");
	if (worker->fd_in != worker->fd_out) {
		if (close(worker->fd_out) < 0)
			i_error("close(worker output) failed: %m");
	}
	aqueue_deinit(&worker->request_queue);
	array_free(&worker->request_array);
	pool_unref(&worker->msg_get_pool);
	i_free(worker);
}

static bool proxy_client_worker_is_output_full(struct dsync_worker *_worker)
{
	struct proxy_client_dsync_worker *worker =
		(struct proxy_client_dsync_worker *)_worker;

	return o_stream_get_buffer_used_size(worker->output) >=
		OUTBUF_THROTTLE_SIZE;
}

static int proxy_client_worker_output_flush(struct dsync_worker *_worker)
{
	struct proxy_client_dsync_worker *worker =
		(struct proxy_client_dsync_worker *)_worker;

	if (o_stream_flush(worker->output) < 0)
		return -1;

	o_stream_uncork(worker->output);
	if (o_stream_get_buffer_used_size(worker->output) > 0)
		return 0;
	o_stream_cork(worker->output);
	return 1;
}

static struct dsync_worker_mailbox_iter *
proxy_client_worker_mailbox_iter_init(struct dsync_worker *_worker)
{
	struct proxy_client_dsync_worker *worker =
		(struct proxy_client_dsync_worker *)_worker;
	struct proxy_client_dsync_worker_mailbox_iter *iter;

	iter = i_new(struct proxy_client_dsync_worker_mailbox_iter, 1);
	iter->iter.worker = _worker;
	iter->pool = pool_alloconly_create("proxy mailbox iter", 1024);
	o_stream_send_str(worker->output, "BOX-LIST\n");
	proxy_client_worker_output_flush(_worker);
	return &iter->iter;
}

static int
proxy_client_worker_mailbox_iter_next(struct dsync_worker_mailbox_iter *_iter,
				      struct dsync_mailbox *dsync_box_r)
{
	struct proxy_client_dsync_worker_mailbox_iter *iter =
		(struct proxy_client_dsync_worker_mailbox_iter *)_iter;
	struct proxy_client_dsync_worker *worker =
		(struct proxy_client_dsync_worker *)_iter->worker;
	const char *line, *error;
	int ret;

	if ((ret = proxy_client_worker_read_line(worker, &line)) <= 0) {
		if (ret < 0)
			_iter->failed = TRUE;
		return ret;
	}

	if (*line == '\t') {
		/* end of mailboxes */
		if (line[1] != '0')
			_iter->failed = TRUE;
		return -1;
	}

	p_clear(iter->pool);
	if (dsync_proxy_mailbox_import(iter->pool, line,
				       dsync_box_r, &error) < 0) {
		i_error("Invalid mailbox input from worker server: %s", error);
		_iter->failed = TRUE;
		return -1;
	}
	return 1;
}

static int
proxy_client_worker_mailbox_iter_deinit(struct dsync_worker_mailbox_iter *_iter)
{
	struct proxy_client_dsync_worker_mailbox_iter *iter =
		(struct proxy_client_dsync_worker_mailbox_iter *)_iter;
	int ret = _iter->failed ? -1 : 0;

	pool_unref(&iter->pool);
	i_free(iter);
	return ret;
}

struct proxy_client_dsync_worker_msg_iter {
	struct dsync_worker_msg_iter iter;
	pool_t pool;
	bool done;
};

static struct dsync_worker_msg_iter *
proxy_client_worker_msg_iter_init(struct dsync_worker *_worker,
				  const mailbox_guid_t mailboxes[],
				  unsigned int mailbox_count)
{
	struct proxy_client_dsync_worker *worker =
		(struct proxy_client_dsync_worker *)_worker;
	struct proxy_client_dsync_worker_msg_iter *iter;
	string_t *str;
	unsigned int i;

	iter = i_new(struct proxy_client_dsync_worker_msg_iter, 1);
	iter->iter.worker = _worker;
	iter->pool = pool_alloconly_create("proxy message iter", 1024);

	str = t_str_new(512);
	str_append(str, "MSG-LIST");
	for (i = 0; i < mailbox_count; i++) {
		str_append_c(str, '\t');
		dsync_proxy_mailbox_guid_export(str, &mailboxes[i]);
	}
	str_append_c(str, '\n');
	o_stream_send(worker->output, str_data(str), str_len(str));
	proxy_client_worker_output_flush(_worker);
	return &iter->iter;
}

static int
proxy_client_worker_msg_iter_next(struct dsync_worker_msg_iter *_iter,
				  unsigned int *mailbox_idx_r,
				  struct dsync_message *msg_r)
{
	struct proxy_client_dsync_worker_msg_iter *iter =
		(struct proxy_client_dsync_worker_msg_iter *)_iter;
	struct proxy_client_dsync_worker *worker =
		(struct proxy_client_dsync_worker *)_iter->worker;
	const char *line, *error;
	int ret;

	if (iter->done)
		return -1;

	if ((ret = proxy_client_worker_read_line(worker, &line)) <= 0) {
		if (ret < 0)
			_iter->failed = TRUE;
		return ret;
	}

	if (*line == '\t') {
		/* end of messages */
		if (line[1] != '0')
			_iter->failed = TRUE;
		iter->done = TRUE;
		return -1;
	}

	*mailbox_idx_r = 0;
	while (*line >= '0' && *line <= '9') {
		*mailbox_idx_r = *mailbox_idx_r * 10 + (*line - '0');
		line++;
	}
	if (*line != '\t') {
		i_error("Invalid mailbox idx from worker server");
		_iter->failed = TRUE;
		return -1;
	}
	line++;

	p_clear(iter->pool);
	if (dsync_proxy_msg_import(iter->pool, line, msg_r, &error) < 0) {
		i_error("Invalid message input from worker server: %s", error);
		_iter->failed = TRUE;
		return -1;
	}
	return 1;
}

static int
proxy_client_worker_msg_iter_deinit(struct dsync_worker_msg_iter *_iter)
{
	struct proxy_client_dsync_worker_msg_iter *iter =
		(struct proxy_client_dsync_worker_msg_iter *)_iter;
	int ret = _iter->failed ? -1 : 0;

	pool_unref(&iter->pool);
	i_free(iter);
	return ret;
}

static void
proxy_client_worker_create_mailbox(struct dsync_worker *_worker,
				   const struct dsync_mailbox *dsync_box)
{
	struct proxy_client_dsync_worker *worker =
		(struct proxy_client_dsync_worker *)_worker;

	T_BEGIN {
		string_t *str = t_str_new(128);

		str_append(str, "BOX-CREATE\t");
		str_tabescape_write(str, dsync_box->name);
		if (dsync_box->uid_validity != 0) {
			str_append_c(str, '\t');
			dsync_proxy_mailbox_guid_export(str, &dsync_box->guid);
			str_printfa(str, "\t%u\n", dsync_box->uid_validity);
		}
		o_stream_send(worker->output, str_data(str), str_len(str));
	} T_END;
}

static void
proxy_client_worker_update_mailbox(struct dsync_worker *_worker,
				   const struct dsync_mailbox *dsync_box)
{
	struct proxy_client_dsync_worker *worker =
		(struct proxy_client_dsync_worker *)_worker;

	T_BEGIN {
		string_t *str = t_str_new(128);

		str_append(str, "BOX-UPDATE\t");
		str_tabescape_write(str, dsync_box->name);
		str_append_c(str, '\t');
		dsync_proxy_mailbox_guid_export(str, &dsync_box->guid);
		str_printfa(str, "\t%u\t%u\t%llu\n",
			    dsync_box->uid_validity, dsync_box->uid_next,
			    (unsigned long long)dsync_box->highest_modseq);
		o_stream_send(worker->output, str_data(str), str_len(str));
	} T_END;
}

static void
proxy_client_worker_select_mailbox(struct dsync_worker *_worker,
				   const mailbox_guid_t *mailbox)
{
	struct proxy_client_dsync_worker *worker =
		(struct proxy_client_dsync_worker *)_worker;

	if (dsync_guid_equals(&worker->selected_box_guid, mailbox))
		return;
	worker->selected_box_guid = *mailbox;

	T_BEGIN {
		string_t *str = t_str_new(128);

		str_append(str, "BOX-SELECT\t");
		dsync_proxy_mailbox_guid_export(str, mailbox);
		str_append_c(str, '\n');
		o_stream_send(worker->output, str_data(str), str_len(str));
	} T_END;
}

static void
proxy_client_worker_msg_update_metadata(struct dsync_worker *_worker,
					const struct dsync_message *msg)
{
	struct proxy_client_dsync_worker *worker =
		(struct proxy_client_dsync_worker *)_worker;

	T_BEGIN {
		string_t *str = t_str_new(128);

		str_printfa(str, "MSG-UPDATE\t%u\t%llu\t", msg->uid,
			    (unsigned long long)msg->modseq);
		imap_write_flags(str, msg->flags & ~MAIL_RECENT, msg->keywords);
		str_append_c(str, '\n');
		o_stream_send(worker->output, str_data(str), str_len(str));
	} T_END;
}

static void
proxy_client_worker_msg_update_uid(struct dsync_worker *_worker,
				   uint32_t old_uid, uint32_t new_uid)
{
	struct proxy_client_dsync_worker *worker =
		(struct proxy_client_dsync_worker *)_worker;

	T_BEGIN {
		o_stream_send_str(worker->output,
			t_strdup_printf("MSG-UID-CHANGE\t%u\t%u\n",
					old_uid, new_uid));
	} T_END;
}

static void
proxy_client_worker_msg_expunge(struct dsync_worker *_worker, uint32_t uid)
{
	struct proxy_client_dsync_worker *worker =
		(struct proxy_client_dsync_worker *)_worker;

	T_BEGIN {
		o_stream_send_str(worker->output,
			t_strdup_printf("MSG-EXPUNGE\t%u\n", uid));
	} T_END;
}

static void
proxy_client_worker_msg_copy(struct dsync_worker *_worker,
			     const mailbox_guid_t *src_mailbox,
			     uint32_t src_uid,
			     const struct dsync_message *dest_msg,
			     dsync_worker_copy_callback_t *callback,
			     void *context)
{
	struct proxy_client_dsync_worker *worker =
		(struct proxy_client_dsync_worker *)_worker;
	struct proxy_client_request request;

	T_BEGIN {
		string_t *str = t_str_new(128);

		str_append(str, "MSG-COPY\t");
		dsync_proxy_mailbox_guid_export(str, src_mailbox);
		str_printfa(str, "\t%u\t", src_uid);
		dsync_proxy_msg_export(str, dest_msg);
		str_append_c(str, '\n');
		o_stream_send(worker->output, str_data(str), str_len(str));
	} T_END;

	memset(&request, 0, sizeof(request));
	request.type = PROXY_CLIENT_REQUEST_TYPE_COPY;
	request.callback.copy = callback;
	request.context = context;
	aqueue_append(worker->request_queue, &request);
}

static void proxy_client_send_stream(struct proxy_client_dsync_worker *worker)
{
	const unsigned char *data;
	size_t size;
	int ret;

	while ((ret = i_stream_read_data(worker->save_input,
					 &data, &size, 0)) > 0) {
		dsync_proxy_send_dot_output(worker->output,
					    &worker->save_input_last_lf,
					    data, size);
		i_stream_skip(worker->save_input, size);

		if (proxy_client_worker_is_output_full(&worker->worker)) {
			o_stream_uncork(worker->output);
			if (proxy_client_worker_is_output_full(&worker->worker))
				return;
			o_stream_cork(worker->output);
		}
	}
	if (ret == 0) {
		/* waiting for more input */
		o_stream_uncork(worker->output);
		if (worker->save_io == NULL) {
			int fd = i_stream_get_fd(worker->save_input);

			worker->save_io =
				io_add(fd, IO_READ,
				       proxy_client_send_stream, worker);
		}
		return;
	}
	if (worker->save_io != NULL)
		io_remove(&worker->save_io);
	if (worker->save_input->stream_errno != 0) {
		errno = worker->save_input->stream_errno;
		i_error("proxy: reading message input failed: %m");
		o_stream_close(worker->output);
	} else {
		i_assert(!i_stream_have_bytes_left(worker->save_input));
		o_stream_send(worker->output, "\n.\n", 3);
	}
	i_stream_unref(&worker->save_input);
}

static void
proxy_client_worker_msg_save(struct dsync_worker *_worker,
			     const struct dsync_message *msg,
			     const struct dsync_msg_static_data *data)
{
	struct proxy_client_dsync_worker *worker =
		(struct proxy_client_dsync_worker *)_worker;

	T_BEGIN {
		string_t *str = t_str_new(128);

		str_append(str, "MSG-SAVE\t");
		dsync_proxy_msg_static_export(str, data);
		str_append_c(str, '\t');
		dsync_proxy_msg_export(str, msg);
		str_append_c(str, '\n');
		o_stream_send(worker->output, str_data(str), str_len(str));
	} T_END;

	i_assert(worker->save_io == NULL);
	i_assert(worker->save_input == NULL);
	worker->save_input = data->input;
	worker->save_input_last_lf = TRUE;
	i_stream_ref(worker->save_input);
	proxy_client_send_stream(worker);
}

static void
proxy_client_worker_msg_get(struct dsync_worker *_worker, uint32_t uid,
			    dsync_worker_msg_callback_t *callback,
			    void *context)
{
	struct proxy_client_dsync_worker *worker =
		(struct proxy_client_dsync_worker *)_worker;
	struct proxy_client_request request;

	T_BEGIN {
		string_t *str = t_str_new(128);

		str_printfa(str, "MSG-GET\t%u\n", uid);
		o_stream_send(worker->output, str_data(str), str_len(str));
	} T_END;

	memset(&request, 0, sizeof(request));
	request.type = PROXY_CLIENT_REQUEST_TYPE_GET;
	request.callback.get = callback;
	request.context = context;
	aqueue_append(worker->request_queue, &request);
}

struct dsync_worker_vfuncs proxy_client_dsync_worker = {
	proxy_client_worker_deinit,

	proxy_client_worker_is_output_full,
	proxy_client_worker_output_flush,

	proxy_client_worker_mailbox_iter_init,
	proxy_client_worker_mailbox_iter_next,
	proxy_client_worker_mailbox_iter_deinit,

	proxy_client_worker_msg_iter_init,
	proxy_client_worker_msg_iter_next,
	proxy_client_worker_msg_iter_deinit,

	proxy_client_worker_create_mailbox,
	proxy_client_worker_update_mailbox,

	proxy_client_worker_select_mailbox,
	proxy_client_worker_msg_update_metadata,
	proxy_client_worker_msg_update_uid,
	proxy_client_worker_msg_expunge,
	proxy_client_worker_msg_copy,
	proxy_client_worker_msg_save,
	proxy_client_worker_msg_get
};
