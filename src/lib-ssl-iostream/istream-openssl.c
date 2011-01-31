/* Copyright (c) 2009 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "istream-internal.h"
#include "iostream-openssl.h"

struct ssl_istream {
	struct istream_private istream;
	struct ssl_iostream *ssl_io;
	bool seen_eof;
};

static void i_stream_ssl_destroy(struct iostream_private *stream)
{
	struct ssl_istream *sstream = (struct ssl_istream *)stream;

	ssl_iostream_unref(&sstream->ssl_io);
}

static ssize_t i_stream_ssl_read(struct istream_private *stream)
{
	struct ssl_istream *sstream = (struct ssl_istream *)stream;
	size_t size;
	ssize_t ret;

	if (sstream->seen_eof) {
		stream->istream.eof = TRUE;
		return -1;
	}
	if (!sstream->ssl_io->handshaked) {
		if ((ret = ssl_iostream_handshake(sstream->ssl_io)) <= 0) {
			if (ret < 0)
				stream->istream.stream_errno = errno;
			return ret;
		}
	}

	if (!i_stream_get_buffer_space(stream, 1, &size))
		return -2;

	while ((ret = SSL_read(sstream->ssl_io->ssl,
			       stream->w_buffer + stream->pos, size)) <= 0) {
		ret = ssl_iostream_handle_error(sstream->ssl_io, ret,
						"SSL_read");
		if (ret <= 0) {
			if (ret < 0) {
				stream->istream.stream_errno = errno;
				stream->istream.eof = TRUE;
				sstream->seen_eof = TRUE;
			}
			return ret;
		}
		(void)ssl_iostream_bio_sync(sstream->ssl_io);
	}
	stream->pos += ret;
	return ret;
}

struct istream *i_stream_create_ssl(struct ssl_iostream *ssl_io)
{
	struct ssl_istream *sstream;

	ssl_io->refcount++;

	sstream = i_new(struct ssl_istream, 1);
	sstream->ssl_io = ssl_io;
	sstream->istream.iostream.destroy = i_stream_ssl_destroy;
	sstream->istream.max_buffer_size =
		ssl_io->plain_input->real_stream->max_buffer_size;
	sstream->istream.read = i_stream_ssl_read;

	sstream->istream.istream.readable_fd = FALSE;
	return i_stream_create(&sstream->istream, NULL, -1);
}
