/* Copyright (C) 2006 Timo Sirainen */

#include "lib.h"
#include "istream-internal.h"
#include "istream-tee.h"

struct tee_istream {
	pool_t pool;
	struct istream *input;
	struct tee_child_istream *children;
};

struct tee_child_istream {
	struct _istream istream;

	struct tee_istream *tee;
	struct tee_child_istream *next;
};

static void tee_streams_update_buffer(struct tee_istream *tee)
{
	struct tee_child_istream *tstream = tee->children;
	const unsigned char *data;
	size_t size, old_used;

	data = i_stream_get_data(tee->input, &size);
	for (; tstream != NULL; tstream = tstream->next) {
		old_used = tstream->istream.pos - tstream->istream.skip;

		tstream->istream.buffer = data;
		tstream->istream.skip = tstream->istream.istream.v_offset -
			tee->input->v_offset;
		i_assert(tstream->istream.skip + old_used <= size);
		tstream->istream.pos = tstream->istream.skip + old_used;
	}
}

static void tee_streams_skip(struct tee_istream *tee)
{
	struct tee_child_istream *tstream = tee->children;
	size_t min_skip;

	min_skip = (size_t)-1;
	for (; tstream != NULL; tstream = tstream->next) {
		if (tstream->istream.skip < min_skip)
			min_skip = tstream->istream.skip;
	}

	if (min_skip > 0) {
		i_stream_skip(tee->input, min_skip);
		tee_streams_update_buffer(tee);
	}
}

static void _close(struct _iostream *stream)
{
	struct tee_child_istream *tstream = (struct tee_child_istream *)stream;

	tee_streams_skip(tstream->tee);
}

static void _destroy(struct _iostream *stream)
{
	struct tee_child_istream *tstream = (struct tee_child_istream *)stream;
	struct tee_istream *tee = tstream->tee;
	struct tee_child_istream **p;

	for (p = &tee->children; *p != NULL; p = &(*p)->next) {
		if (*p == tstream) {
			*p = tstream->next;
			break;
		}
	}

	if (tee->children == NULL) {
		i_stream_unref(&tee->input);
		p_free(tee->pool, tee);
	}
}

static void _set_max_buffer_size(struct _iostream *stream, size_t max_size)
{
	struct tee_child_istream *tstream = (struct tee_child_istream *)stream;

	return i_stream_set_max_buffer_size(tstream->tee->input, max_size);
}

static ssize_t _read(struct _istream *stream)
{
	struct tee_child_istream *tstream = (struct tee_child_istream *)stream;
	struct istream *input = tstream->tee->input;
	const unsigned char *data;
	size_t size;
	uoff_t last_high_offset;
	ssize_t ret;

	data = i_stream_get_data(input, &size);

	last_high_offset = stream->istream.v_offset +
		(tstream->istream.pos - tstream->istream.skip);
	i_assert(last_high_offset <= input->v_offset + size);
	if (last_high_offset == input->v_offset + size) {
		tee_streams_skip(tstream->tee);
		ret = i_stream_read(input);
		if (ret <= 0) {
			data = i_stream_get_data(input, &size);
			if (ret == -2 && stream->skip != 0) {
				/* someone else is holding the data,
				   wait for it */
				return 0;
			}
			stream->istream.eof = input->eof;
			return ret;
		}
		tee_streams_update_buffer(tstream->tee);
		data = i_stream_get_data(input, &size);
	} else if (stream->buffer == NULL) {
		tee_streams_update_buffer(tstream->tee);
	}

	i_assert(stream->buffer == data);
	ret = size - stream->pos;
	stream->pos = size;
	return ret;
}

static void _seek(struct _istream *stream __attr_unused__,
		  uoff_t v_offset __attr_unused__,
		  bool mark __attr_unused__)
{
	i_panic("tee-istream: seeking unsupported currently");
}

static const struct stat *_stat(struct _istream *stream, bool exact)
{
	struct tee_child_istream *tstream = (struct tee_child_istream *)stream;

	return i_stream_stat(tstream->tee->input, exact);
}

static void _sync(struct _istream *stream)
{
	struct tee_child_istream *tstream = (struct tee_child_istream *)stream;
	size_t size;

	tee_streams_skip(tstream->tee);
	(void)i_stream_get_data(tstream->tee->input, &size);
	if (size != 0) {
		i_panic("tee-istream: i_stream_sync() called "
			"with data still buffered");
	}
	return i_stream_sync(tstream->tee->input);
}

struct tee_istream *tee_i_stream_create(struct istream *input, pool_t pool)
{
	struct tee_istream *tee;

	tee = p_new(pool, struct tee_istream, 1);
	tee->pool = pool;
	tee->input = input;

	i_stream_ref(input);
	return tee;
}

struct istream *
tee_i_stream_create_child(struct tee_istream *tee, pool_t pool)
{
	struct tee_child_istream *tstream;

	tstream = p_new(pool, struct tee_child_istream, 1);
	tstream->tee = tee;

	tstream->istream.iostream.close = _close;
	tstream->istream.iostream.destroy = _destroy;
	tstream->istream.iostream.set_max_buffer_size = _set_max_buffer_size;

	tstream->istream.read = _read;
	tstream->istream.seek = _seek;
	tstream->istream.stat = _stat;
	tstream->istream.sync = _sync;

	tstream->next = tee->children;
	tee->children = tstream;

	return _i_stream_create(&tstream->istream, pool,
				i_stream_get_fd(tee->input), 0);
}
