/* Copyright (c) 2002-2003 Timo Sirainen */

#include "lib.h"
#include "mmap-util.h"
#include "istream-internal.h"

#include <unistd.h>
#include <sys/stat.h>

struct mmap_istream {
	struct _istream istream;

	int fd;
	void *mmap_base;
	off_t mmap_offset;
	size_t mmap_block_size;
	uoff_t v_size;

	unsigned int autoclose_fd:1;
};

static size_t mmap_pagemask = 0;

static void _close(struct _iostream *stream)
{
	struct mmap_istream *mstream = (struct mmap_istream *) stream;

	if (mstream->autoclose_fd && mstream->fd != -1) {
		if (close(mstream->fd) < 0)
			i_error("mmap_istream.close() failed: %m");
		mstream->fd = -1;
	}
}

static void i_stream_munmap(struct mmap_istream *mstream)
{
	struct _istream *_stream = &mstream->istream;

	if (_stream->buffer != NULL) {
		if (munmap(mstream->mmap_base, _stream->buffer_size) < 0)
			i_error("mmap_istream.munmap() failed: %m");
		mstream->mmap_base = NULL;
		_stream->buffer = NULL;
		_stream->buffer_size = 0;
		mstream->mmap_offset = 0;
	}
}

static void _destroy(struct _iostream *stream)
{
	struct mmap_istream *mstream = (struct mmap_istream *) stream;

	i_stream_munmap(mstream);
}

static void _set_max_buffer_size(struct _iostream *stream, size_t max_size)
{
	struct mmap_istream *mstream = (struct mmap_istream *) stream;

	/* allow only full page sizes */
	if (max_size < mmap_get_page_size())
		mstream->mmap_block_size = mmap_get_page_size();
	else {
		if (max_size % mmap_get_page_size() != 0) {
			max_size += mmap_get_page_size() -
				(max_size % mmap_get_page_size());
		}
		mstream->mmap_block_size = max_size;
	}
}

static ssize_t _read(struct _istream *stream)
{
	struct mmap_istream *mstream = (struct mmap_istream *) stream;
	size_t aligned_skip;
	uoff_t top;

	stream->istream.stream_errno = 0;

	if (stream->pos < stream->buffer_size) {
		/* more bytes available without needing to mmap() */
		stream->pos = stream->buffer_size;
		return stream->pos - stream->skip;
	}

	if (stream->istream.v_offset >= mstream->v_size) {
		stream->istream.eof = TRUE;
		return -1;
	}

	aligned_skip = stream->skip & ~mmap_pagemask;
	if (aligned_skip == 0 && mstream->mmap_base != NULL) {
		/* didn't skip enough bytes */
		return -2;
	}

	stream->skip -= aligned_skip;
	mstream->mmap_offset += aligned_skip;

	if (mstream->mmap_base != NULL) {
		if (munmap(mstream->mmap_base, stream->buffer_size) < 0)
			i_error("io_stream_read_mmaped(): munmap() failed: %m");
	}

	top = mstream->v_size - mstream->mmap_offset;
	stream->buffer_size = I_MIN(top, mstream->mmap_block_size);

	i_assert((uoff_t)mstream->mmap_offset + stream->buffer_size <=
		 mstream->v_size);

	if (stream->buffer_size == 0) {
		/* don't bother even trying mmap */
		mstream->mmap_base = NULL;
		stream->buffer = NULL;
	} else {
		mstream->mmap_base =
			mmap(NULL, stream->buffer_size, PROT_READ, MAP_PRIVATE,
			     mstream->fd, mstream->mmap_offset);
		if (mstream->mmap_base == MAP_FAILED) {
			stream->istream.stream_errno = errno;
			mstream->mmap_base = NULL;
			stream->buffer = NULL;
			stream->buffer_size = 0;
			stream->skip = stream->pos = 0;
			i_error("mmap_istream.mmap() failed: %m");
			return -1;
		}
		stream->buffer = mstream->mmap_base;
	}

	if (stream->buffer_size > mmap_get_page_size()) {
		if (madvise(mstream->mmap_base, stream->buffer_size,
			    MADV_SEQUENTIAL) < 0)
			i_error("mmap_istream.madvise(): %m");
	}

	stream->pos = stream->buffer_size;
	i_assert(stream->pos - stream->skip != 0);
	return stream->pos - stream->skip;
}

static void _seek(struct _istream *stream, uoff_t v_offset)
{
	struct mmap_istream *mstream = (struct mmap_istream *) stream;

	if (stream->buffer_size != 0 &&
	    (uoff_t)mstream->mmap_offset <= v_offset &&
	    (uoff_t)mstream->mmap_offset + stream->buffer_size > v_offset) {
		/* already mmaped */
		stream->skip = stream->pos = v_offset - mstream->mmap_offset;
	} else {
		/* force reading next time */
		i_stream_munmap(mstream);
		stream->skip = stream->pos = v_offset;
	}

	stream->istream.v_offset = v_offset;
}

static uoff_t _get_size(struct _istream *stream)
{
	struct mmap_istream *mstream = (struct mmap_istream *) stream;

	return mstream->v_size;
}

struct istream *i_stream_create_mmap(int fd, pool_t pool, size_t block_size,
				     uoff_t start_offset, uoff_t v_size,
				     int autoclose_fd)
{
	struct mmap_istream *mstream;
        struct istream *istream;
	struct stat st;

	if (mmap_pagemask == 0)
		mmap_pagemask = mmap_get_page_size()-1;

	if (v_size == 0) {
		if (fstat(fd, &st) < 0)
			i_error("i_stream_create_mmap(): fstat() failed: %m");
		else {
			v_size = st.st_size;
			if (start_offset > v_size)
				start_offset = v_size;
			v_size -= start_offset;
		}
	}

	mstream = p_new(pool, struct mmap_istream, 1);
	mstream->fd = fd;
        _set_max_buffer_size(&mstream->istream.iostream, block_size);
	mstream->autoclose_fd = autoclose_fd;
	mstream->v_size = v_size;

	mstream->istream.iostream.close = _close;
	mstream->istream.iostream.destroy = _destroy;
	mstream->istream.iostream.set_max_buffer_size = _set_max_buffer_size;

	mstream->istream.read = _read;
	mstream->istream.seek = _seek;
	mstream->istream.get_size = _get_size;

	istream = _i_stream_create(&mstream->istream, pool, fd, start_offset);
	istream->mmaped = TRUE;
	istream->seekable = TRUE;
	return istream;
}
