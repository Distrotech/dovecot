#ifndef __IOBUFFER_INTERNAL_H
#define __IOBUFFER_INTERNAL_H

#include "ioloop.h" /* TimeoutFunc */

/* This file is private to IBuffer and OBuffer implementation */

typedef struct _IOBuffer _IOBuffer;

struct _IOBuffer {
	Pool pool;
	int refcount;

	void (*close)(_IOBuffer *buf);
	void (*destroy)(_IOBuffer *buf);
	void (*set_max_size)(_IOBuffer *buf, size_t max_size);
	void (*set_blocking)(_IOBuffer *buf, int timeout_msecs,
			     TimeoutFunc timeout_func, void *context);
};

void _io_buffer_init(Pool pool, _IOBuffer *buf);
void _io_buffer_ref(_IOBuffer *buf);
void _io_buffer_unref(_IOBuffer *buf);
void _io_buffer_close(_IOBuffer *buf);
void _io_buffer_set_max_size(_IOBuffer *buf, size_t max_size);
void _io_buffer_set_blocking(_IOBuffer *buf, int timeout_msecs,
			     TimeoutFunc timeout_func, void *context);

#endif
