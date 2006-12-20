#ifndef __FTS_API_PRIVATE_H
#define __FTS_API_PRIVATE_H

#include "fts-api.h"

struct fts_backend_vfuncs {
	struct fts_backend *(*init)(struct mailbox *box);
	void (*deinit)(struct fts_backend *backend);

	int (*get_last_uid)(struct fts_backend *backend, uint32_t *last_uid_r);

	struct fts_backend_build_context *
		(*build_init)(struct fts_backend *backend,
			      uint32_t *last_uid_r);
	int (*build_more)(struct fts_backend_build_context *ctx, uint32_t uid,
			  const unsigned char *data, size_t size);
	int (*build_deinit)(struct fts_backend_build_context *ctx);

	void (*expunge)(struct fts_backend *backend, struct mail *mail);
	void (*expunge_finish)(struct fts_backend *backend,
			       struct mailbox *box, bool committed);

	int (*lock)(struct fts_backend *backend);
	void (*unlock)(struct fts_backend *backend);

	int (*lookup)(struct fts_backend *backend, const char *key,
		      ARRAY_TYPE(seq_range) *result);
	int (*filter)(struct fts_backend *backend, const char *key,
		      ARRAY_TYPE(seq_range) *result);
};

enum fts_backend_flags {
	/* If set, lookup() and filter() are trusted to return only
	   actual matches. Otherwise the returned mails are opened and
	   searched. */
	FTS_BACKEND_FLAG_DEFINITE_LOOKUPS	= 0x01,
	/* If set, the backend is used also for TEXT and BODY search
	   optimizations. Otherwise only TEXT_FAST and BODY_FAST are
	   optimized. */
	FTS_BACKEND_FLAG_EXACT_LOOKUPS		= 0x02
};

struct fts_backend {
	const char *name;
	enum fts_backend_flags flags;

	struct fts_backend_vfuncs v;
};

struct fts_backend_build_context {
	struct fts_backend *backend;

	unsigned int failed:1;
};

void fts_backend_register(const struct fts_backend *backend);
void fts_backend_unregister(const char *name);

#endif
