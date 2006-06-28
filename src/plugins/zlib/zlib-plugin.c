/* Copyright (C) 2005 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "istream-zlib.h"
#include "home-expand.h"
#include "istream.h"
#include "mail-storage-private.h"
#include "zlib-plugin.h"

#include <fcntl.h>

struct zlib_mail_storage {
	struct mail_storage_vfuncs super;
};

#define ZLIB_CONTEXT(obj) \
	*((void **)array_idx_modifiable(&(obj)->module_contexts, \
					zlib_storage_module_id))

/* defined by imap, pop3, lda */
extern void (*hook_mail_storage_created)(struct mail_storage *storage);

static void (*zlib_next_hook_mail_storage_created)
	(struct mail_storage *storage);

static unsigned int zlib_storage_module_id = 0;
static bool zlib_storage_module_id_set = FALSE;

static struct mailbox *
zlib_mailbox_open(struct mail_storage *storage, const char *name,
		  struct istream *input, enum mailbox_open_flags flags)
{
	struct zlib_mail_storage *qstorage = ZLIB_CONTEXT(storage);
	struct mailbox *box;
	struct istream *zlib_input = NULL;
	size_t len = strlen(name);

	if (input == NULL && len > 3 && strcmp(name + len - 3, ".gz") == 0) {
		/* Looks like a .gz file */
		const char *path;
		bool is_file;

		path = mail_storage_get_mailbox_path(storage, name, &is_file);
		if (is_file && path != NULL) {
			/* it's a single file mailbox. we can handle this. */
			int fd;

			fd = open(path, O_RDONLY);
			if (fd != -1) {
				input = zlib_input =
					i_stream_create_zlib(fd, default_pool);
			}
		}
	}

	box = qstorage->super.mailbox_open(storage, name, input, flags);

	if (zlib_input != NULL)
		i_stream_unref(&zlib_input);

	return box;
}

static void zlib_mail_storage_created(struct mail_storage *storage)
{
	struct zlib_mail_storage *qstorage;

	if (zlib_next_hook_mail_storage_created != NULL)
		zlib_next_hook_mail_storage_created(storage);

	qstorage = p_new(storage->pool, struct zlib_mail_storage, 1);
	qstorage->super = storage->v;
	storage->v.mailbox_open = zlib_mailbox_open;

	if (!zlib_storage_module_id_set) {
		zlib_storage_module_id = mail_storage_module_id++;
		zlib_storage_module_id_set = TRUE;
	}

	array_idx_set(&storage->module_contexts,
		      zlib_storage_module_id, &qstorage);
}

void zlib_plugin_init(void)
{
	zlib_next_hook_mail_storage_created =
		hook_mail_storage_created;
	hook_mail_storage_created = zlib_mail_storage_created;
}

void zlib_plugin_deinit(void)
{
	hook_mail_storage_created =
		zlib_next_hook_mail_storage_created;
}
