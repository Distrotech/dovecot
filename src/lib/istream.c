/* Copyright (c) 2002-2003 Timo Sirainen */

#include "lib.h"
#include "istream-internal.h"

void i_stream_ref(struct istream *stream)
{
	_io_stream_ref(&stream->real_stream->iostream);
}

void i_stream_unref(struct istream *stream)
{
	_io_stream_unref(&stream->real_stream->iostream);
}

int i_stream_get_fd(struct istream *stream)
{
	struct _istream *_stream = stream->real_stream;

	return _stream->fd;
}

void i_stream_close(struct istream *stream)
{
	_io_stream_close(&stream->real_stream->iostream);
	stream->closed = TRUE;
}

void i_stream_set_max_buffer_size(struct istream *stream, size_t max_size)
{
	_io_stream_set_max_buffer_size(&stream->real_stream->iostream,
				       max_size);
}

void i_stream_set_blocking(struct istream *stream, int timeout_msecs,
			   void (*timeout_cb)(void *), void *context)
{
	_io_stream_set_blocking(&stream->real_stream->iostream, timeout_msecs,
				timeout_cb, context);
}

ssize_t i_stream_read(struct istream *stream)
{
	struct _istream *_stream = stream->real_stream;

	if (stream->closed)
		return -1;

	stream->disconnected = FALSE;
	return _stream->read(_stream);
}

void i_stream_skip(struct istream *stream, uoff_t count)
{
	struct _istream *_stream = stream->real_stream;
	size_t data_size;

	data_size = _stream->pos - _stream->skip;
	if (count <= data_size) {
		/* within buffer */
		stream->v_offset += count;
		_stream->skip += count;
		return;
	}

	/* have to seek forward */
	count -= data_size;
	_stream->skip = _stream->pos;
	stream->v_offset += data_size;

	if (stream->closed)
		return;

	_stream->seek(_stream, stream->v_offset + count);
}

void i_stream_seek(struct istream *stream, uoff_t v_offset)
{
	struct _istream *_stream = stream->real_stream;

	if (v_offset >= stream->v_offset) {
		i_stream_skip(stream, v_offset - stream->v_offset);
		return;
	}

	if (stream->closed)
		return;

	stream->disconnected = FALSE;
	_stream->seek(_stream, v_offset);
}

uoff_t i_stream_get_size(struct istream *stream)
{
	struct _istream *_stream = stream->real_stream;

	return _stream->get_size(_stream);
}

char *i_stream_next_line(struct istream *stream)
{
	struct _istream *_stream = stream->real_stream;
	char *ret_buf;
        size_t i;

        i_assert(stream != NULL);

	if (_stream->skip >= _stream->pos) {
		stream->stream_errno = 0;
		return NULL;
	}

	if (_stream->w_buffer == NULL) {
		i_error("i_stream_next_line() called for unmodifyable stream");
		return NULL;
	}

	/* @UNSAFE */
	ret_buf = NULL;
	for (i = _stream->skip; i < _stream->pos; i++) {
		if (_stream->buffer[i] == 10) {
			/* got it */
			if (i > 0 && _stream->buffer[i-1] == '\r')
				_stream->w_buffer[i-1] = '\0';
			else
				_stream->w_buffer[i] = '\0';
			ret_buf = (char *) _stream->w_buffer + _stream->skip;

			i++;
			stream->v_offset += i - _stream->skip;
			_stream->skip = i;
                        break;
		}
	}

        return ret_buf;
}

char *i_stream_read_next_line(struct istream *stream)
{
	char *line;

	line = i_stream_next_line(stream);
	if (line != NULL)
		return line;

	if (i_stream_read(stream) > 0)
		line = i_stream_next_line(stream);
	return line;
}

const unsigned char *i_stream_get_data(struct istream *stream, size_t *size)
{
	struct _istream *_stream = stream->real_stream;

	if (_stream->skip >= _stream->pos) {
		*size = 0;
		return NULL;
	}

        *size = _stream->pos - _stream->skip;
        return _stream->buffer + _stream->skip;
}

unsigned char *i_stream_get_modifyable_data(struct istream *stream,
					    size_t *size)
{
	struct _istream *_stream = stream->real_stream;

	if (_stream->skip >= _stream->pos || _stream->w_buffer == NULL) {
		*size = 0;
		return NULL;
	}

        *size = _stream->pos - _stream->skip;
        return _stream->w_buffer + _stream->skip;
}

int i_stream_read_data(struct istream *stream, const unsigned char **data,
		       size_t *size, size_t threshold)
{
	ssize_t ret = 0;

	do {
		*data = i_stream_get_data(stream, size);
		if (*size > threshold)
			return 1;

		/* we need more data */
		ret = i_stream_read(stream);
	} while (ret >= 0);

	return ret == -2 ? -2 :
		*size > 0 ? 0 : -1;
}

struct istream *_i_stream_create(struct _istream *_stream, pool_t pool, int fd,
				 uoff_t abs_start_offset)
{
	_stream->fd = fd;
	_stream->abs_start_offset = abs_start_offset;
	_stream->istream.real_stream = _stream;

	_io_stream_init(pool, &_stream->iostream);
	return &_stream->istream;
}

#ifdef STREAM_TEST
/* gcc istream.c -o teststream liblib.a -Wall -DHAVE_CONFIG_H -DSTREAM_TEST -g */

#include <fcntl.h>
#include <unistd.h>
#include "ostream.h"

#define BUF_VALUE(offset) \
        (((offset) % 256) ^ ((offset) / 256))

static void check_buffer(const unsigned char *data, size_t size, size_t offset)
{
	size_t i;

	for (i = 0; i < size; i++)
		i_assert(data[i] == BUF_VALUE(i+offset));
}

int main(void)
{
	struct istream *input, *l_input;
	struct ostream *output1, *output2;
	int i, fd1, fd2;
	unsigned char buf[1024];
	const unsigned char *data;
	size_t size;

	lib_init();

	fd1 = open("teststream.1", O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd1 < 0)
		i_fatal("open() failed: %m");
	fd2 = open("teststream.2", O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd2 < 0)
		i_fatal("open() failed: %m");

	/* write initial data */
	for (i = 0; i < sizeof(buf); i++)
		buf[i] = BUF_VALUE(i);
	write(fd1, buf, sizeof(buf));

	/* test reading */
	input = i_stream_create_file(fd1, default_pool, 512, FALSE);
	i_assert(i_stream_get_size(input) == sizeof(buf));

	i_assert(i_stream_read_data(input, &data, &size, 0) > 0);
	i_assert(size == 512);
	check_buffer(data, size, 0);

	i_stream_seek(input, 256);
	i_assert(i_stream_read_data(input, &data, &size, 0) > 0);
	i_assert(size == 512);
	check_buffer(data, size, 256);

	i_stream_seek(input, 0);
	i_assert(i_stream_read_data(input, &data, &size, 512) == -2);
	i_assert(size == 512);
	check_buffer(data, size, 0);

	i_stream_skip(input, 900);
	i_assert(i_stream_read_data(input, &data, &size, 0) > 0);
	i_assert(size == sizeof(buf) - 900);
	check_buffer(data, size, 900);

	/* test moving data */
	output1 = o_stream_create_file(fd1, default_pool, 512, FALSE);
	output2 = o_stream_create_file(fd2, default_pool, 512, FALSE);

	i_stream_seek(input, 1); size = sizeof(buf)-1;
	i_assert(o_stream_send_istream(output2, input) == size);
	o_stream_flush(output2);

	lseek(fd2, 0, SEEK_SET);
	i_assert(read(fd2, buf, sizeof(buf)) == size);
	check_buffer(buf, size, 1);

	i_stream_seek(input, 0);
	o_stream_seek(output1, sizeof(buf));
	i_assert(o_stream_send_istream(output1, input) == sizeof(buf));

	/* test moving with limits */
	l_input = i_stream_create_limit(default_pool, input,
					sizeof(buf)/2, 512);
	i_stream_seek(l_input, 0);
	o_stream_seek(output1, 10);
	i_assert(o_stream_send_istream(output1, l_input) == 512);

	i_stream_set_max_buffer_size(input, sizeof(buf));

	i_stream_seek(input, 0);
	i_assert(i_stream_read_data(input, &data, &size, sizeof(buf)-1) > 0);
	i_assert(size == sizeof(buf));
	check_buffer(data, 10, 0);
	check_buffer(data + 10, 512, sizeof(buf)/2);
	check_buffer(data + 10 + 512,
		     size - (10 + 512), 10 + 512);

	/* reading within limits */
	i_stream_seek(l_input, 0);
	i_assert(i_stream_read_data(l_input, &data, &size, 511) > 0);
	i_assert(size == 512);
	i_assert(i_stream_read_data(l_input, &data, &size, 512) == -2);
	i_assert(size == 512);
	i_stream_skip(l_input, 511);
	i_assert(i_stream_read_data(l_input, &data, &size, 0) > 0);
	i_assert(size == 1);
	i_stream_skip(l_input, 1);
	i_assert(i_stream_read_data(l_input, &data, &size, 0) == -1);
	i_assert(size == 0);

	unlink("teststream.1");
	unlink("teststream.2");
	return 0;
}
#endif
