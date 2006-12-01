/* Copyright (C) 2002-2003 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "istream.h"
#include "mkdir-parents.h"
#include "unlink-directory.h"
#include "mbox-storage.h"
#include "mbox-lock.h"
#include "mbox-file.h"
#include "mbox-sync-private.h"
#include "mail-copy.h"
#include "index-mail.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define CREATE_MODE 0770 /* umask() should limit it more */

/* Assume that if atime < mtime, there are new mails. If it's good enough for
   UW-IMAP, it's good enough for us. */
#define STAT_GET_MARKED(st) \
	((st).st_size == 0 ? MAILBOX_UNMARKED : \
	 (st).st_atime < (st).st_mtime ? MAILBOX_MARKED : MAILBOX_UNMARKED)

/* NOTE: must be sorted for istream-header-filter. Note that it's not such
   a good idea to change this list, as the messages will then change from
   client's point of view. So if you do it, change all mailboxes' UIDVALIDITY
   so all caches are reset. */
const char *mbox_hide_headers[] = {
	"Content-Length",
	"Status",
	"X-IMAP",
	"X-IMAPbase",
	"X-Keywords",
	"X-Status",
	"X-UID"
};
unsigned int mbox_hide_headers_count =
	sizeof(mbox_hide_headers) / sizeof(mbox_hide_headers[0]);

extern struct mail_storage mbox_storage;
extern struct mailbox mbox_mailbox;

static bool mbox_mailbox_list_module_id_set = FALSE;
static unsigned int mbox_mailbox_list_module_id;

int mbox_set_syscall_error(struct mbox_mailbox *mbox, const char *function)
{
	i_assert(function != NULL);

	mail_storage_set_critical(STORAGE(mbox->storage),
		"%s failed with mbox file %s: %m", function, mbox->path);
	return -1;
}

static bool mbox_handle_errors(struct index_storage *istorage)
{
	struct mail_storage *storage = &istorage->storage;

	if (ENOACCESS(errno))
		mail_storage_set_error(storage, MAIL_STORAGE_ERR_NO_PERMISSION);
	else if (ENOSPACE(errno))
		mail_storage_set_error(storage, "Not enough disk space");
	else if (ENOTFOUND(errno))
		mail_storage_set_error(storage, "Directory structure is broken");
	else
		return FALSE;
	return TRUE;
}

static bool mbox_is_file(const char *path, const char *name, bool debug)
{
	struct stat st;

	if (stat(path, &st) < 0) {
		if (debug) {
			i_info("mbox autodetect: %s: stat(%s) failed: %m",
			       name, path);
		}
		return FALSE;
	}
	if (S_ISDIR(st.st_mode)) {
		if (debug) {
			i_info("mbox autodetect: %s: is a directory (%s)",
			       name, path);
		}
		return FALSE;
	}
	if (access(path, R_OK|W_OK) < 0) {
		if (debug) {
			i_info("mbox autodetect: %s: no R/W access (%s)",
			       name, path);
		}
		return FALSE;
	}

	if (debug)
		i_info("mbox autodetect: %s: yes (%s)", name, path);
	return TRUE;
}

static bool mbox_is_dir(const char *path, const char *name, bool debug)
{
	struct stat st;

	if (stat(path, &st) < 0) {
		if (debug) {
			i_info("mbox autodetect: %s: stat(%s) failed: %m",
			       name, path);
		}
		return FALSE;
	}
	if (!S_ISDIR(st.st_mode)) {
		if (debug) {
			i_info("mbox autodetect: %s: is not a directory (%s)",
			       name, path);
		}
		return FALSE;
	}
	if (access(path, R_OK|W_OK|X_OK) < 0) {
		if (debug) {
			i_info("mbox autodetect: %s: no R/W/X access (%s)",
			       name, path);
		}
		return FALSE;
	}

	if (debug)
		i_info("mbox autodetect: %s: yes (%s)", name, path);
	return TRUE;
}

static bool mbox_autodetect(const char *data, enum mail_storage_flags flags)
{
	bool debug = (flags & MAIL_STORAGE_FLAG_DEBUG) != 0;
	const char *path;

	path = t_strcut(data, ':');

	if (debug) {
		if (strchr(data, ':') != NULL) {
			i_info("mbox autodetect: data=%s, splitting ':' -> %s",
			       data, path);
		} else {
			i_info("mbox autodetect: data=%s", data);
		}
	}

	if (*path != '\0' && mbox_is_file(path, "INBOX file", debug))
		return TRUE;

	if (mbox_is_dir(t_strconcat(path, "/"MBOX_INDEX_DIR_NAME, NULL),
			"has "MBOX_INDEX_DIR_NAME"/", debug))
		return TRUE;
	if (mbox_is_file(t_strconcat(path, "/inbox", NULL), "has inbox", debug))
		return TRUE;
	if (mbox_is_file(t_strconcat(path, "/mbox", NULL), "has mbox", debug))
		return TRUE;

	return FALSE;
}

static const char *get_root_dir(enum mail_storage_flags flags)
{
	const char *home, *path;
	bool debug = (flags & MAIL_STORAGE_FLAG_DEBUG) != 0;

	home = getenv("HOME");
	if (home != NULL) {
		path = t_strconcat(home, "/mail", NULL);
		if (access(path, R_OK|W_OK|X_OK) == 0) {
			if (debug)
				i_info("mbox: root exists (%s)", path);
			return path;
		}
		if (debug)
			i_info("mbox: root: access(%s, rwx) failed: %m", path);

		path = t_strconcat(home, "/Mail", NULL);
		if (access(path, R_OK|W_OK|X_OK) == 0) {
			if (debug)
				i_info("mbox: root exists (%s)", path);
			return path;
		}
		if (debug)
			i_info("mbox: root: access(%s, rwx) failed: %m", path);
	}

	if (debug)
		i_info("mbox: checking if we are chrooted:");
	if (mbox_autodetect("", flags))
		return "/";

	if (debug)
		i_info("mbox: root mail directory not found");

	return NULL;
}

static const char *
get_inbox_file(const char *root_dir, bool only_root, bool debug)
{
	const char *user, *path;

	if (!only_root && (user = getenv("USER")) != NULL) {
		path = t_strconcat("/var/mail/", user, NULL);
		if (access(path, R_OK|W_OK) == 0) {
			if (debug)
				i_info("mbox: INBOX exists (%s)", path);
			return path;
		}
		if (debug)
			i_info("mbox: INBOX: access(%s, rw) failed: %m", path);

		path = t_strconcat("/var/spool/mail/", user, NULL);
		if (access(path, R_OK|W_OK) == 0) {
			if (debug)
				i_info("mbox: INBOX exists (%s)", path);
			return path;
		}
		if (debug)
			i_info("mbox: INBOX: access(%s, rw) failed: %m", path);
	}

	path = t_strconcat(root_dir, "/inbox", NULL);
	if (debug)
		i_info("mbox: INBOX defaulted to %s", path);
	return path;
}

static const char *create_root_dir(bool debug)
{
	const char *home, *path;

	home = getenv("HOME");
	if (home == NULL) {
		i_error("mbox: We need root mail directory, "
			"but can't find it or HOME environment");
		return NULL;
	}

	path = t_strconcat(home, "/mail", NULL);
	if (mkdir_parents(path, CREATE_MODE) < 0) {
		i_error("mbox: Can't create root mail directory %s: %m", path);
		return NULL;
	}

	if (debug)
		i_info("mbox: root directory created: %s", path);
	return path;
}

static int
mbox_get_list_settings(struct mailbox_list_settings *list_set,
		       const char *data, enum mail_storage_flags flags)
{
	bool debug = (flags & MAIL_STORAGE_FLAG_DEBUG) != 0;
	const char *p;
	struct stat st;
	bool autodetect;

	memset(list_set, 0, sizeof(*list_set));
	list_set->subscription_fname = MBOX_SUBSCRIPTION_FILE_NAME;
	list_set->maildir_name = "";

	autodetect = data == NULL || *data == '\0';
	if (autodetect) {
		if ((flags & MAIL_STORAGE_FLAG_NO_AUTODETECTION) != 0) {
			i_error("mbox: root mail directory not given");
			return -1;
		}

		/* we'll need to figure out the mail location ourself.
		   it's root dir if we've already chroot()ed, otherwise
		   either $HOME/mail or $HOME/Mail */
		list_set->root_dir = get_root_dir(flags);
	} else {
		/* <root mail directory> | <INBOX path>
		   [:INBOX=<path>] [:INDEX=<dir>] */
		if (debug)
			i_info("mbox: data=%s", data);
		p = strchr(data, ':');
		if (p == NULL) {
			/* if the data points to a file, treat it as an INBOX */
			if ((flags & MAIL_STORAGE_FLAG_NO_AUTODETECTION) != 0 ||
			    stat(data, &st) < 0 || S_ISDIR(st.st_mode))
				list_set->root_dir = data;
			else {
				list_set->root_dir = get_root_dir(flags);
				list_set->inbox_path = data;
			}
		} else {
			list_set->root_dir = t_strdup_until(data, p);
			do {
				p++;
				if (strncmp(p, "INBOX=", 6) == 0) {
					list_set->inbox_path =
						t_strcut(p+6, ':');
				} else if (strncmp(p, "INDEX=", 6) == 0) {
					list_set->index_dir =
						t_strcut(p+6, ':');
				}
				p = strchr(p, ':');
			} while (p != NULL);
		}
	}

	if (list_set->root_dir == NULL) {
		list_set->root_dir = create_root_dir(debug);
		if (list_set->root_dir == NULL)
			return -1;
	} else {
		/* strip trailing '/' */
		size_t len = strlen(list_set->root_dir);

		if (list_set->root_dir[len-1] == '/') {
			list_set->root_dir =
				t_strndup(list_set->root_dir, len-1);
		}

		/* make sure the directory exists */
		if (*list_set->root_dir == '\0' ||
		    lstat(list_set->root_dir, &st) == 0) {
			/* yep, go ahead */
		} else if (errno != ENOENT && errno != ENOTDIR) {
			i_error("lstat(%s) failed: %m", list_set->root_dir);
			return -1;
		} else if (mkdir_parents(list_set->root_dir, CREATE_MODE) < 0 &&
			   errno != EEXIST) {
			i_error("mkdir_parents(%s) failed: %m",
				list_set->root_dir);
			return -1;
		}
	}

	if (list_set->inbox_path == NULL) {
		list_set->inbox_path =
			get_inbox_file(list_set->root_dir, !autodetect, debug);
	}

	if (list_set->index_dir != NULL &&
	    strcmp(list_set->index_dir, "MEMORY") == 0)
		list_set->index_dir = "";
	return 0;
}

static const char *
mbox_list_get_path(struct mailbox_list *list, const char *name,
		   enum mailbox_list_path_type type)
{
	struct mbox_storage *storage =
		*((void **)array_idx_modifiable(&list->module_contexts,
						mbox_mailbox_list_module_id));
	const char *path, *p;

	path = storage->list_super.get_path(list, name, type);
	if (type == MAILBOX_LIST_PATH_TYPE_CONTROL ||
	    type == MAILBOX_LIST_PATH_TYPE_INDEX) {
		p = strrchr(path, '/');
		if (p == NULL)
			return "";

		return t_strconcat(t_strdup_until(path, p),
				   "/"MBOX_INDEX_DIR_NAME"/", p+1, NULL);
	}
	return path;
}

static struct mail_storage *
mbox_create(const char *data, const char *user, enum mail_storage_flags flags,
	    enum mail_storage_lock_method lock_method)
{
	struct mbox_storage *storage;
	struct index_storage *istorage;
	struct mailbox_list_settings list_set;
	struct mailbox_list *list;
	const char *error;
	pool_t pool;

	if (mbox_get_list_settings(&list_set, data, flags) < 0)
		return NULL;
	list_set.mail_storage_flags = &flags;
	list_set.mail_storage_lock_method = &lock_method;

	pool = pool_alloconly_create("storage", 512);
	storage = p_new(pool, struct mbox_storage, 1);

	if (mailbox_list_init("fs", &list_set,
			      mail_storage_get_list_flags(flags),
			      mailbox_storage_list_is_mailbox, storage,
			      &list, &error) < 0) {
		i_error("mbox fs: %s", error);
		pool_unref(pool);
		return NULL;
	}

	storage->list_super = list->v;
	list->v.get_path = mbox_list_get_path;

	if (!mbox_mailbox_list_module_id_set) {
		mbox_mailbox_list_module_id_set = TRUE;
		mbox_mailbox_list_module_id = mailbox_list_module_id++;
	}
	array_idx_set(&list->module_contexts,
		      mbox_mailbox_list_module_id, &storage);

	istorage = INDEX_STORAGE(storage);
	istorage->storage = mbox_storage;
	istorage->storage.pool = pool;

	istorage->user = p_strdup(pool, user);
	istorage->callbacks = p_new(pool, struct mail_storage_callbacks, 1);
	index_storage_init(istorage, list, flags, lock_method);
	return &storage->storage.storage;
}

static void mbox_free(struct mail_storage *_storage)
{
	struct index_storage *storage = (struct index_storage *)_storage;

	index_storage_deinit(storage);
	pool_unref(storage->storage.pool);
}

static int create_mbox_index_dirs(struct mail_storage *storage,
				  const char *name)
{
	const char *index_dir;

	index_dir = mailbox_list_get_path(storage->list, name,
					  MAILBOX_LIST_PATH_TYPE_INDEX);
	if (*index_dir == '\0')
		return 0;

	if (mkdir_parents(index_dir, CREATE_MODE) < 0) {
		mail_storage_set_critical(storage,
			"mkdir_parents(%s) failed: %m", index_dir);
		return -1;
	}

	return 0;
}

static int verify_inbox(struct mail_storage *storage)
{
	const char *inbox_path;
	int fd;

	inbox_path = mailbox_list_get_path(storage->list, "INBOX",
					   MAILBOX_LIST_PATH_TYPE_MAILBOX);

	/* make sure inbox file itself exists */
	fd = open(inbox_path, O_RDWR | O_CREAT | O_EXCL, 0660);
	if (fd != -1)
		(void)close(fd);
	else if (errno != EEXIST) {
		mail_storage_set_critical(storage,
			"open(%s, O_CREAT) failed: %m", inbox_path);
	}

	return 0;
}

static bool mbox_mail_is_recent(struct index_mailbox *ibox __attr_unused__,
				uint32_t uid __attr_unused__)
{
	return FALSE;
}

static bool want_memory_indexes(struct mbox_storage *storage, const char *path)
{
	const char *env;
	struct stat st;
	unsigned int min_size;

	env = getenv("MBOX_MIN_INDEX_SIZE");
	if (env == NULL)
		return FALSE;

	min_size = strtoul(env, NULL, 10);
	if (min_size == 0)
		return FALSE;

	if (stat(path, &st) < 0) {
		if (errno == ENOENT)
			st.st_size = 0;
		else {
			mail_storage_set_critical(STORAGE(storage),
						  "stat(%s) failed: %m", path);
			return FALSE;
		}
	}
	return st.st_size / 1024 < min_size;
}

static struct mbox_mailbox *
mbox_alloc(struct mbox_storage *storage, struct mail_index *index,
	   const char *name, const char *path, enum mailbox_open_flags flags)
{
	struct mbox_mailbox *mbox;
	pool_t pool;

	pool = pool_alloconly_create("mailbox", 1024);
	mbox = p_new(pool, struct mbox_mailbox, 1);
	mbox->ibox.box = mbox_mailbox;
	mbox->ibox.box.pool = pool;
	mbox->ibox.storage = INDEX_STORAGE(storage);
	mbox->ibox.mail_vfuncs = &mbox_mail_vfuncs;
	mbox->ibox.is_recent = mbox_mail_is_recent;

	if (index_storage_mailbox_init(&mbox->ibox, index, name, flags,
				want_memory_indexes(storage, path)) < 0) {
		/* the memory is already freed here, no need to deinit */
		return NULL;
	}

	mbox->storage = storage;
	mbox->mbox_fd = -1;
	mbox->mbox_lock_type = F_UNLCK;
	mbox->mbox_ext_idx =
		mail_index_ext_register(index, "mbox", 0,
					sizeof(uint64_t), sizeof(uint64_t));

        mbox->mbox_very_dirty_syncs = getenv("MBOX_VERY_DIRTY_SYNCS") != NULL;
	mbox->mbox_do_dirty_syncs = mbox->mbox_very_dirty_syncs ||
		getenv("MBOX_DIRTY_SYNCS") != NULL;

	if ((STORAGE(storage)->flags & MAIL_STORAGE_FLAG_KEEP_HEADER_MD5) != 0)
		mbox->mbox_save_md5 = TRUE;
	return mbox;
}

static struct mailbox *
mbox_open(struct mbox_storage *storage, const char *name,
	  enum mailbox_open_flags flags)
{
	struct mail_storage *_storage = STORAGE(storage);
	struct mbox_mailbox *mbox;
	struct mail_index *index;
	const char *path, *index_dir;

	path = mailbox_list_get_path(_storage->list, name,
				     MAILBOX_LIST_PATH_TYPE_MAILBOX);
	index_dir = mailbox_list_get_path(_storage->list, name,
					  MAILBOX_LIST_PATH_TYPE_INDEX);

	if ((flags & MAILBOX_OPEN_NO_INDEX_FILES) != 0)
		index_dir = "";

	if (*index_dir != '\0') {
		/* make sure the index directories exist */
		if (create_mbox_index_dirs(_storage, name) < 0)
			return NULL;
	}

	index = index_storage_alloc(index_dir, path, MBOX_INDEX_PREFIX);
	mbox = mbox_alloc(storage, index, name, path, flags);
	if (mbox == NULL)
		return NULL;

	mbox->path = p_strdup(mbox->ibox.box.pool, path);

	if (access(path, R_OK|W_OK) < 0) {
		if (errno < EACCES)
			mbox_set_syscall_error(mbox, "access()");
		else {
			mbox->ibox.readonly = TRUE;
			mbox->mbox_readonly = TRUE;
		}
	}

	if (mbox->ibox.keep_locked) {
		if (mbox_lock(mbox, F_WRLCK, &mbox->mbox_global_lock_id) <= 0) {
			struct mailbox *box = &mbox->ibox.box;

			mailbox_close(&box);
			return NULL;
		}
	}

	return &mbox->ibox.box;
}

static struct mailbox *
mbox_mailbox_open_stream(struct mbox_storage *storage, const char *name,
			 struct istream *input, enum mailbox_open_flags flags)
{
	struct mail_storage *_storage = STORAGE(storage);
	struct mail_index *index;
	struct mbox_mailbox *mbox;
	const char *path, *index_dir;

	flags |= MAILBOX_OPEN_READONLY;

	path = mailbox_list_get_path(_storage->list, name,
				     MAILBOX_LIST_PATH_TYPE_MAILBOX);
	if ((flags & MAILBOX_OPEN_NO_INDEX_FILES) != 0)
		index_dir = "";
	else {
		index_dir = mailbox_list_get_path(_storage->list, name,
						  MAILBOX_LIST_PATH_TYPE_INDEX);

		/* make sure the required directories are also there */
		if (create_mbox_index_dirs(_storage, name) < 0)
			return NULL;
	}

	index = index_storage_alloc(index_dir, path, MBOX_INDEX_PREFIX);
	mbox = mbox_alloc(storage, index, name, path, flags);
	if (mbox == NULL)
		return NULL;

	i_stream_ref(input);
	mbox->mbox_file_stream = input;
	mbox->mbox_readonly = TRUE;
	mbox->no_mbox_file = TRUE;

	mbox->path = "(read-only mbox stream)";
	return &mbox->ibox.box;
}

static struct mailbox *
mbox_mailbox_open(struct mail_storage *_storage, const char *name,
		  struct istream *input, enum mailbox_open_flags flags)
{
	struct mbox_storage *storage = (struct mbox_storage *)_storage;
	struct index_storage *istorage = INDEX_STORAGE(storage);
	const char *path;
	struct stat st;

	mail_storage_clear_error(_storage);

	if (input != NULL)
		return mbox_mailbox_open_stream(storage, name, input, flags);

	if (strcmp(name, "INBOX") == 0) {
		/* make sure INBOX exists */
		if (verify_inbox(_storage) < 0)
			return NULL;
		return mbox_open(storage, "INBOX", flags);
	}

	if (!mailbox_list_is_valid_existing_name(_storage->list, name)) {
		mail_storage_set_error(_storage, "Invalid mailbox name");
		return NULL;
	}

	path = mailbox_list_get_path(_storage->list, name,
				     MAILBOX_LIST_PATH_TYPE_MAILBOX);
	if (stat(path, &st) == 0) {
		if (S_ISDIR(st.st_mode)) {
			mail_storage_set_error(_storage,
				"Mailbox isn't selectable: %s", name);
			return NULL;
		}

		return mbox_open(storage, name, flags);
	}

	if (ENOTFOUND(errno)) {
		mail_storage_set_error(_storage,
			MAIL_STORAGE_ERR_MAILBOX_NOT_FOUND, name);
	} else if (!mbox_handle_errors(istorage)) {
		mail_storage_set_critical(_storage, "stat(%s) failed: %m",
					  path);
	}

	return NULL;
}

static int mbox_mailbox_create(struct mail_storage *_storage, const char *name,
			       bool directory)
{
	struct index_storage *storage = (struct index_storage *)_storage;
	const char *path, *p;
	struct stat st;
	int fd;

	mail_storage_clear_error(_storage);

	if (!mailbox_list_is_valid_create_name(_storage->list, name)) {
		mail_storage_set_error(_storage, "Invalid mailbox name");
		return -1;
	}

	if (strncasecmp(name, "INBOX/", 6) == 0) {
		/* We might actually be able to create mailboxes under INBOX
		   because the real INBOX file isn't usually named as INBOX
		   in the root mail directory. that would anyway be a special
		   case which would require special handling elsewhere, so just
		   don't allow it. */
		mail_storage_set_error(_storage,
				"Mailbox doesn't allow inferior mailboxes");
		return -1;
	}

	/* make sure it doesn't exist already */
	path = mailbox_list_get_path(_storage->list, name,
				     MAILBOX_LIST_PATH_TYPE_MAILBOX);
	if (stat(path, &st) == 0) {
		mail_storage_set_error(_storage, "Mailbox already exists");
		return -1;
	}

	if (errno != ENOENT) {
		if (errno == ENOTDIR) {
			mail_storage_set_error(_storage,
				"Mailbox doesn't allow inferior mailboxes");
		} else if (!mbox_handle_errors(storage)) {
			mail_storage_set_critical(_storage,
				"stat() failed for mbox file %s: %m", path);
		}
		return -1;
	}

	/* create the hierarchy if needed */
	p = directory ? path + strlen(path) : strrchr(path, '/');
	if (p != NULL) {
		p = t_strdup_until(path, p);
		if (mkdir_parents(p, CREATE_MODE) < 0) {
			if (mbox_handle_errors(storage))
				return -1;

			mail_storage_set_critical(_storage,
				"mkdir_parents(%s) failed: %m", p);
			return -1;
		}

		if (directory) {
			/* wanted to create only the directory */
			return 0;
		}
	}

	/* create the mailbox file */
	fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0660);
	if (fd != -1) {
		(void)close(fd);
		return 0;
	}

	if (errno == EEXIST) {
		/* mailbox was just created between stat() and open() call.. */
		mail_storage_set_error(_storage, "Mailbox already exists");
	} else if (!mbox_handle_errors(storage)) {
		mail_storage_set_critical(_storage,
			"Can't create mailbox %s: %m", name);
	}
	return -1;
}

static int mbox_mailbox_delete(struct mail_storage *_storage, const char *name)
{
	struct index_storage *storage = (struct index_storage *)_storage;
	const char *index_dir, *path;
	struct stat st;

	mail_storage_clear_error(_storage);

	if (strcmp(name, "INBOX") == 0) {
		mail_storage_set_error(_storage, "INBOX can't be deleted.");
		return -1;
	}

	if (!mailbox_list_is_valid_existing_name(_storage->list, name)) {
		mail_storage_set_error(_storage, "Invalid mailbox name");
		return -1;
	}

	path = mailbox_list_get_path(_storage->list, name,
				     MAILBOX_LIST_PATH_TYPE_MAILBOX);
	if (lstat(path, &st) < 0) {
		if (ENOTFOUND(errno)) {
			mail_storage_set_error(_storage,
				MAIL_STORAGE_ERR_MAILBOX_NOT_FOUND, name);
		} else if (!mbox_handle_errors(storage)) {
			mail_storage_set_critical(_storage,
				"lstat() failed for %s: %m", path);
		}
		return -1;
	}

	if (S_ISDIR(st.st_mode)) {
		/* deleting a directory. allow it only if it doesn't contain
		   anything. Delete the ".imap" directory first in case there
		   have been indexes. */
		index_dir = mailbox_list_get_path(_storage->list, name,
					MAILBOX_LIST_PATH_TYPE_MAILBOX);

		if (*index_dir != '\0' && rmdir(index_dir) < 0 &&
		    !ENOTFOUND(errno) && errno != ENOTEMPTY) {
			if (!mbox_handle_errors(storage)) {
				mail_storage_set_critical(_storage,
					"rmdir() failed for %s: %m", index_dir);
				return -1;
			}
		}

		if (rmdir(path) == 0)
			return 0;

		if (ENOTFOUND(errno)) {
			mail_storage_set_error(_storage,
				MAIL_STORAGE_ERR_MAILBOX_NOT_FOUND, name);
		} else if (errno == ENOTEMPTY) {
			mail_storage_set_error(_storage,
				"Folder %s isn't empty, can't delete it.",
				name);
		} else if (!mbox_handle_errors(storage)) {
			mail_storage_set_critical(_storage,
				"rmdir() failed for %s: %m", path);
		}
		return -1;
	}

	/* delete the index directory first, so that if we crash we don't
	   leave indexes for deleted mailboxes lying around */
	index_dir = mailbox_list_get_path(_storage->list, name,
					  MAILBOX_LIST_PATH_TYPE_INDEX);
	if (*index_dir != '\0') {
		index_storage_destroy_unrefed();

		if (unlink_directory(index_dir, TRUE) < 0 && errno != ENOENT) {
			mail_storage_set_critical(_storage,
				"unlink_directory(%s) failed: %m", index_dir);
			return -1;
		}
	}

	if (unlink(path) < 0) {
		if (ENOTFOUND(errno)) {
			mail_storage_set_error(_storage,
				MAIL_STORAGE_ERR_MAILBOX_NOT_FOUND, name);
		} else if (!mbox_handle_errors(storage)) {
			mail_storage_set_critical(_storage,
				"unlink() failed for %s: %m", path);
		}
		return -1;
	}

	return 0;
}

static int mbox_mailbox_rename(struct mail_storage *_storage,
			       const char *oldname, const char *newname)
{
	struct index_storage *storage = (struct index_storage *)_storage;
	const char *oldpath, *newpath, *old_indexdir, *new_indexdir, *p;
	struct stat st;

	mail_storage_clear_error(_storage);

	if (!mailbox_list_is_valid_existing_name(_storage->list, oldname) ||
	    !mailbox_list_is_valid_create_name(_storage->list, newname)) {
		mail_storage_set_error(_storage, "Invalid mailbox name");
		return -1;
	}

	if (strncasecmp(newname, "INBOX/", 6) == 0) {
		/* Not allowed - see explanation in mbox_mailbox_create */
		mail_storage_set_error(_storage,
			"Target mailbox doesn't allow inferior mailboxes");
		return -1;
	}

	oldpath = mailbox_list_get_path(_storage->list, oldname,
					MAILBOX_LIST_PATH_TYPE_MAILBOX);
	newpath = mailbox_list_get_path(_storage->list, newname,
					MAILBOX_LIST_PATH_TYPE_MAILBOX);

	/* create the hierarchy */
	p = strrchr(newpath, '/');
	if (p != NULL) {
		p = t_strdup_until(newpath, p);
		if (mkdir_parents(p, CREATE_MODE) < 0) {
			if (mbox_handle_errors(storage))
				return -1;

			mail_storage_set_critical(_storage,
				"mkdir_parents(%s) failed: %m", p);
			return -1;
		}
	}

	/* first check that the destination mailbox doesn't exist.
	   this is racy, but we need to be atomic and there's hardly any
	   possibility that someone actually tries to rename two mailboxes
	   to same new one */
	if (lstat(newpath, &st) == 0) {
		mail_storage_set_error(_storage,
				       "Target mailbox already exists");
		return -1;
	} else if (errno == ENOTDIR) {
		mail_storage_set_error(_storage,
			"Target mailbox doesn't allow inferior mailboxes");
		return -1;
	} else if (errno != ENOENT && errno != EACCES) {
		mail_storage_set_critical(_storage, "lstat(%s) failed: %m",
					  newpath);
		return -1;
	}

	/* NOTE: renaming INBOX works just fine with us, it's simply recreated
	   the next time it's needed. */
	if (rename(oldpath, newpath) < 0) {
		if (ENOTFOUND(errno)) {
			mail_storage_set_error(_storage,
				MAIL_STORAGE_ERR_MAILBOX_NOT_FOUND, oldname);
		} else if (!mbox_handle_errors(storage)) {
			mail_storage_set_critical(_storage,
				"rename(%s, %s) failed: %m", oldpath, newpath);
		}
		return -1;
	}

	/* we need to rename the index directory as well */
	old_indexdir = mailbox_list_get_path(_storage->list, oldname,
					     MAILBOX_LIST_PATH_TYPE_INDEX);
	new_indexdir = mailbox_list_get_path(_storage->list, newname,
					     MAILBOX_LIST_PATH_TYPE_INDEX);
	if (*old_indexdir != '\0') {
		if (rename(old_indexdir, new_indexdir) < 0 &&
		    errno != ENOENT) {
			mail_storage_set_critical(_storage,
						  "rename(%s, %s) failed: %m",
						  old_indexdir, new_indexdir);
		}
	}

	return 0;
}

static int mbox_storage_close(struct mailbox *box)
{
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)box;
	const struct mail_index_header *hdr;
	int ret = 0;

	hdr = mail_index_get_header(mbox->ibox.view);
	if ((hdr->flags & MAIL_INDEX_HDR_FLAG_HAVE_DIRTY) != 0 &&
	    !mbox->mbox_readonly) {
		/* we've done changes to mbox which haven't been written yet.
		   do it now. */
		if (mbox_sync(mbox, MBOX_SYNC_REWRITE) < 0)
			ret = -1;
	}

	if (mbox->mbox_global_lock_id != 0)
		(void)mbox_unlock(mbox, mbox->mbox_global_lock_id);

        mbox_file_close(mbox);
	if (mbox->mbox_file_stream != NULL)
		i_stream_destroy(&mbox->mbox_file_stream);

	index_storage_mailbox_free(box);
	return ret;
}

static void
mbox_notify_changes(struct mailbox *box, unsigned int min_interval,
		    mailbox_notify_callback_t *callback, void *context)
{
	struct mbox_mailbox *mbox = (struct mbox_mailbox *)box;

	mbox->ibox.min_notify_interval = min_interval;
	mbox->ibox.notify_callback = callback;
	mbox->ibox.notify_context = context;

	if (callback == NULL)
		index_mailbox_check_remove_all(&mbox->ibox);
	else if (!mbox->no_mbox_file)
		index_mailbox_check_add(&mbox->ibox, mbox->path);
}

static int mbox_is_mailbox(struct mail_storage *storage,
			   const char *dir, const char *fname,
			   enum mailbox_list_iter_flags iter_flags,
			   enum mailbox_info_flags *flags,
			   enum mailbox_list_file_type type)
{
	const char *path, *root_dir;
	size_t len;
	struct stat st;
	int ret = 1;

	if (strcmp(fname, MBOX_INDEX_DIR_NAME) == 0) {
		*flags = MAILBOX_NOSELECT;
		return 0;
	}
	if (strcmp(fname, MBOX_SUBSCRIPTION_FILE_NAME) == 0) {
		root_dir = mailbox_list_get_path(storage->list, NULL,
					MAILBOX_LIST_PATH_TYPE_MAILBOX);
		if (strcmp(root_dir, dir) == 0) {
			*flags = MAILBOX_NOSELECT | MAILBOX_NOINFERIORS;
			return 0;
		}
	}

	/* skip all .lock files */
	len = strlen(fname);
	if (len > 5 && strcmp(fname+len-5, ".lock") == 0) {
		*flags = MAILBOX_NOSELECT | MAILBOX_NOINFERIORS;
		return 0;
	}

	/* try to avoid stat() with these checks */
	if (type == MAILBOX_LIST_FILE_TYPE_DIR) {
		*flags |= MAILBOX_NOSELECT | MAILBOX_CHILDREN;
		return 1;
	}
	if (type != MAILBOX_LIST_FILE_TYPE_SYMLINK &&
	    type != MAILBOX_LIST_FILE_TYPE_UNKNOWN &&
	    (iter_flags & MAILBOX_LIST_ITER_FAST_FLAGS) != 0) {
		*flags |= MAILBOX_NOINFERIORS;
		return 1;
	}

	/* need to stat() then */
	t_push();
	path = t_strconcat(dir, "/", fname, NULL);
	if (stat(path, &st) == 0) {
		if (S_ISDIR(st.st_mode))
			*flags |= MAILBOX_NOSELECT | MAILBOX_CHILDREN;
		else
			*flags |= MAILBOX_NOINFERIORS | STAT_GET_MARKED(st);
	} else if (errno == EACCES || errno == ELOOP)
		*flags |= MAILBOX_NOSELECT;
	else if (ENOTFOUND(errno))
		ret = 0;
	else {
		mail_storage_set_critical(storage, "stat(%s) failed: %m", path);
		ret = -1;
	}
	t_pop();

	return ret;
}

static void mbox_class_init(void)
{
	mbox_transaction_class_init();
}

static void mbox_class_deinit(void)
{
	mbox_transaction_class_deinit();
}

struct mail_storage mbox_storage = {
	MEMBER(name) MBOX_STORAGE_NAME,
	MEMBER(mailbox_is_file) TRUE,

	{
		mbox_class_init,
		mbox_class_deinit,
		mbox_create,
		mbox_free,
		mbox_autodetect,
		index_storage_set_callbacks,
		mbox_mailbox_open,
		mbox_mailbox_create,
		mbox_mailbox_delete,
		mbox_mailbox_rename,
		mbox_is_mailbox,
		index_storage_get_last_error
	}
};

struct mailbox mbox_mailbox = {
	MEMBER(name) NULL, 
	MEMBER(storage) NULL, 

	{
		index_storage_is_readonly,
		index_storage_allow_new_keywords,
		mbox_storage_close,
		index_storage_get_status,
		mbox_storage_sync_init,
		index_mailbox_sync_next,
		index_mailbox_sync_deinit,
		mbox_notify_changes,
		index_transaction_begin,
		index_transaction_commit,
		index_transaction_rollback,
		index_keywords_create,
		index_keywords_free,
		index_storage_get_uids,
		index_mail_alloc,
		index_header_lookup_init,
		index_header_lookup_deinit,
		index_storage_search_init,
		index_storage_search_deinit,
		index_storage_search_next,
		index_storage_search_next_update_seq,
		mbox_save_init,
		mbox_save_continue,
		mbox_save_finish,
		mbox_save_cancel,
		mail_storage_copy,
		index_storage_is_inconsistent
	}
};
