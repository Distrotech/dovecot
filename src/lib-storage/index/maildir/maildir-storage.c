/* Copyright (C) 2002-2006 Timo Sirainen */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "hostpid.h"
#include "str.h"
#include "home-expand.h"
#include "mkdir-parents.h"
#include "unlink-directory.h"
#include "maildir-storage.h"
#include "maildir-uidlist.h"
#include "maildir-keywords.h"
#include "maildir-sync.h"
#include "index-mail.h"

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>

#define CREATE_MODE 0777 /* umask() should limit it more */

#define MAILDIR_PLUSPLUS_DRIVER_NAME "maildir++"
#define MAILDIR_SUBFOLDER_FILENAME "maildirfolder"

#define MAILDIR_LIST_CONTEXT(obj) \
	MODULE_CONTEXT(obj, maildir_mailbox_list_module)

struct rename_context {
	bool found;
	size_t oldnamelen;
	const char *newname;
};

extern struct mail_storage maildir_storage;
extern struct mailbox maildir_mailbox;

static MODULE_CONTEXT_DEFINE_INIT(maildir_mailbox_list_module,
				  &mailbox_list_module_register);

static int verify_inbox(struct mail_storage *storage,
			enum mailbox_open_flags *flags);
static int
maildir_list_delete_mailbox(struct mailbox_list *list, const char *name);
static int
maildir_list_rename_mailbox(struct mailbox_list *list,
			    const char *oldname, const char *newname);
static int
maildir_list_iter_is_mailbox(struct mailbox_list_iterate_context *ctx
			     	__attr_unused__,
			     const char *dir, const char *fname,
			     enum mailbox_list_file_type type,
			     enum mailbox_info_flags *flags_r);
static int
maildirplusplus_iter_is_mailbox(struct mailbox_list_iterate_context *ctx,
				const char *dir, const char *fname,
				enum mailbox_list_file_type type,
				enum mailbox_info_flags *flags_r);

static const char *strip_tail_slash(const char *path)
{
	size_t len = strlen(path);

	if (len > 0 && path[len-1] == '/')
		return t_strndup(path, len-1);
	else
		return path;
}

static const char *strip_tail_slash_and_cut(const char *path)
{
	return strip_tail_slash(t_strcut(path, ':'));
}

static int
maildir_get_list_settings(struct mailbox_list_settings *list_set,
			  const char *data, enum mail_storage_flags flags,
			  const char **layout_r)
{
	bool debug = (flags & MAIL_STORAGE_FLAG_DEBUG) != 0;
	const char *home, *path, *p;

	*layout_r = MAILDIR_PLUSPLUS_DRIVER_NAME;

	memset(list_set, 0, sizeof(*list_set));
	list_set->subscription_fname = MAILDIR_SUBSCRIPTION_FILE_NAME;
	list_set->maildir_name = "";

	if (data == NULL || *data == '\0') {
		if ((flags & MAIL_STORAGE_FLAG_NO_AUTODETECTION) != 0) {
			i_error("maildir: root directory not given");
			return -1;
		}

		/* we'll need to figure out the maildir location ourself.
		   It's $HOME/Maildir unless we are chrooted. */
		if ((home = getenv("HOME")) != NULL) {
			path = t_strconcat(home, "/Maildir", NULL);
			if (access(path, R_OK|W_OK|X_OK) == 0) {
				if (debug) {
					i_info("maildir: root exists (%s)",
					       path);
				}
				list_set->root_dir = path;
			} else {
				if (debug) {
					i_info("maildir: access(%s, rwx): "
					       "failed: %m", path);
				}
			}
		} else {
			if (debug)
				i_info("maildir: HOME not set");
		}

		if (access("/cur", R_OK|W_OK|X_OK) == 0) {
			if (debug)
				i_info("maildir: /cur exists, assuming chroot");
			list_set->root_dir = "/";
		}
	} else {
		/* <Maildir> [:INBOX=<dir>] [:INDEX=<dir>] [:CONTROL=<dir>] */
		if (debug)
			i_info("maildir: data=%s", data);
		p = strchr(data, ':');
		if (p == NULL)
			list_set->root_dir = data;
		else {
			list_set->root_dir = t_strdup_until(data, p);

			do {
				p++;
				if (strncmp(p, "INBOX=", 6) == 0) {
					list_set->inbox_path =
						strip_tail_slash_and_cut(p+6);
				} else if (strncmp(p, "INDEX=", 6) == 0) {
					list_set->index_dir =
						strip_tail_slash_and_cut(p+6);
				} else if (strncmp(p, "CONTROL=", 8) == 0) {
					list_set->control_dir =
						strip_tail_slash_and_cut(p+8);
				} else if (strncmp(p, "LAYOUT=", 7) == 0) {
					*layout_r =
						strip_tail_slash_and_cut(p+7);
				}
				p = strchr(p, ':');
			} while (p != NULL);
		}
	}

	if (list_set->root_dir == NULL) {
		if (debug)
			i_info("maildir: couldn't find root dir");
		return -1;
	}
	list_set->root_dir = strip_tail_slash(list_set->root_dir);
	if (list_set->inbox_path == NULL)
		list_set->inbox_path = list_set->root_dir;

	if (list_set->index_dir != NULL &&
	    strcmp(list_set->index_dir, "MEMORY") == 0)
		list_set->index_dir = "";
	return 0;
}

static bool maildir_is_internal_name(const char *name)
{
	return strcmp(name, "cur") == 0 ||
		strcmp(name, "new") == 0 ||
		strcmp(name, "tmp") == 0;
}

static bool maildir_storage_is_valid_existing_name(struct mailbox_list *list,
						   const char *name)
{
	struct maildir_storage *storage = MAILDIR_LIST_CONTEXT(list);
	const char *p;

	if (!storage->list_module_ctx.super.is_valid_existing_name(list, name))
		return FALSE;

	/* Don't allow the mailbox name to end in cur/new/tmp */
	p = strrchr(name, '/');
	if (p != NULL)
		name = p + 1;
	return !maildir_is_internal_name(name);
}

static bool maildir_storage_is_valid_create_name(struct mailbox_list *list,
						 const char *name)
{
	struct maildir_storage *storage = MAILDIR_LIST_CONTEXT(list);
	const char *const *tmp;
	bool ret = TRUE;

	if (!storage->list_module_ctx.super.is_valid_create_name(list, name))
		return FALSE;

	/* Don't allow creating mailboxes under cur/new/tmp */
	t_push();
	for (tmp = t_strsplit(name, "/"); *tmp != NULL; tmp++) {
		if (maildir_is_internal_name(*tmp)) {
			ret = FALSE;
			break;
		}
	}
	t_pop();
	return ret;
}

static struct mail_storage *
maildir_create(const char *data, const char *user,
	       enum mail_storage_flags flags,
	       enum file_lock_method lock_method)
{
	struct maildir_storage *storage;
	struct mailbox_list_settings list_set;
	struct mailbox_list *list;
	enum mailbox_open_flags open_flags;
	const char *layout, *error;
	struct stat st;
	pool_t pool;

	if (maildir_get_list_settings(&list_set, data, flags, &layout) < 0)
		return NULL;
	list_set.mail_storage_flags = &flags;
	list_set.lock_method = &lock_method;

	/* normally the maildir is created in verify_inbox() */
	if ((flags & MAIL_STORAGE_FLAG_NO_AUTOCREATE) != 0) {
		if (stat(list_set.root_dir, &st) < 0) {
			if (errno != ENOENT) {
				i_error("stat(%s) failed: %m",
					list_set.root_dir);
			}
			return NULL;
		}
	}

	pool = pool_alloconly_create("storage", 512+256);
	storage = p_new(pool, struct maildir_storage, 1);

	if (mailbox_list_init(layout, &list_set,
			      mail_storage_get_list_flags(flags),
			      &list, &error) < 0) {
		i_error("maildir %s: %s", layout, error);
		pool_unref(pool);
		return NULL;
	}
	storage->list_module_ctx.super = list->v;
	if (strcmp(layout, MAILDIR_PLUSPLUS_DRIVER_NAME) == 0) {
		list->v.iter_is_mailbox = maildirplusplus_iter_is_mailbox;
	} else {
		list->v.is_valid_existing_name =
			maildir_storage_is_valid_existing_name;
		list->v.is_valid_create_name =
			maildir_storage_is_valid_create_name;
		list->v.iter_is_mailbox = maildir_list_iter_is_mailbox;
	}
	list->v.delete_mailbox = maildir_list_delete_mailbox;
	list->v.rename_mailbox = maildir_list_rename_mailbox;

	MODULE_CONTEXT_SET_FULL(list, maildir_mailbox_list_module,
				storage, &storage->list_module_ctx);

	storage->copy_with_hardlinks =
		getenv("MAILDIR_COPY_WITH_HARDLINKS") != NULL;
	storage->copy_preserve_filename =
		getenv("MAILDIR_COPY_PRESERVE_FILENAME") != NULL;
	storage->stat_dirs = getenv("MAILDIR_STAT_DIRS") != NULL;

	storage->temp_prefix = mailbox_list_get_temp_prefix(list);
	if (list_set.control_dir == NULL) {
		/* put the temp files into tmp/ directory preferrably */
		storage->temp_prefix =
			p_strconcat(pool, "tmp/", storage->temp_prefix, NULL);
	}

	storage->storage = maildir_storage;
	storage->storage.pool = pool;
	storage->storage.user = p_strdup(pool, user);
	index_storage_init(&storage->storage, list, flags, lock_method);

	open_flags = 0;
	(void)verify_inbox(&storage->storage, &open_flags);
	return &storage->storage;
}

static void maildir_free(struct mail_storage *storage)
{
	index_storage_deinit(storage);
	pool_unref(storage->pool);
}

static bool maildir_autodetect(const char *data, enum mail_storage_flags flags)
{
	bool debug = (flags & MAIL_STORAGE_FLAG_DEBUG) != 0;
	struct stat st;
	const char *path;

	data = t_strcut(data, ':');

	path = t_strconcat(data, "/cur", NULL);
	if (stat(path, &st) < 0) {
		if (debug)
			i_info("maildir autodetect: stat(%s) failed: %m", path);
		return FALSE;
	}

	if (!S_ISDIR(st.st_mode)) {
		if (debug)
			i_info("maildir autodetect: %s not a directory", path);
		return FALSE;
	}
	return TRUE;
}

static int mkdir_verify(struct mail_storage *storage,
			const char *dir, bool verify)
{
	struct stat st;

	if (verify) {
		if (stat(dir, &st) == 0)
			return 0;

		if (errno != ENOENT) {
			mail_storage_set_critical(storage,
						  "stat(%s) failed: %m", dir);
			return -1;
		}
	}

	if (mkdir_parents(dir, CREATE_MODE) < 0) {
		if (errno == EEXIST) {
			if (!verify)
				return -1;
		} else {
			mail_storage_set_critical(storage,
						  "mkdir(%s) failed: %m", dir);
			return -1;
		}
	}
	return 0;
}

/* create or fix maildir, ignore if it already exists */
static int create_maildir(struct mail_storage *storage,
			  const char *dir, bool verify)
{
	const char *path;
	struct stat st;

	if (mkdir_verify(storage, t_strconcat(dir, "/cur", NULL), verify) < 0)
		return -1;
	if (mkdir_verify(storage, t_strconcat(dir, "/new", NULL), verify) < 0)
		return -1;

	/* if tmp/ directory exists, we need to clean it up once in a while */
	path = t_strconcat(dir, "/tmp", NULL);
	if (stat(path, &st) == 0) {
		if (st.st_atime >
		    st.st_ctime + MAILDIR_TMP_DELETE_SECS) {
			/* the directory should be empty. we won't do anything
			   until ctime changes. */
		} else if (st.st_atime < ioloop_time - MAILDIR_TMP_SCAN_SECS) {
			/* time to scan */
			(void)maildir_tmp_cleanup(storage, path);
		}
	} else if (errno == ENOENT) {
		if (mkdir_verify(storage, path, verify) < 0)
			return -1;
	} else {
		mail_storage_set_critical(storage, "stat(%s) failed: %m", path);
		return -1;
	}

	return 0;
}

static int create_index_dir(struct mail_storage *storage, const char *name)
{
	const char *index_dir, *root_dir, *dir;

	index_dir = mailbox_list_get_path(storage->list, name,
					  MAILBOX_LIST_PATH_TYPE_INDEX);
	if (*index_dir == '\0')
		return 0;

	root_dir = mailbox_list_get_path(storage->list, name,
					 MAILBOX_LIST_PATH_TYPE_MAILBOX);
	if (strcmp(index_dir, root_dir) == 0)
		return 0;

	dir = t_strdup_printf("%s/%c%s", index_dir,
			      mailbox_list_get_hierarchy_sep(storage->list),
			      name);
	if (mkdir_parents(dir, CREATE_MODE) < 0 && errno != EEXIST) {
		if (!ENOSPACE(errno)) {
			mail_storage_set_critical(storage,
						  "mkdir(%s) failed: %m", dir);
		}
		return -1;
	}

	return 0;
}

static int create_control_dir(struct mail_storage *storage, const char *name)
{
	const char *control_dir, *root_dir, *dir;

	control_dir = mailbox_list_get_path(storage->list, name,
					    MAILBOX_LIST_PATH_TYPE_CONTROL);
	root_dir = mailbox_list_get_path(storage->list, name,
					 MAILBOX_LIST_PATH_TYPE_MAILBOX);
	if (strcmp(control_dir, root_dir) == 0)
		return 0;

	dir = t_strdup_printf("%s/%c%s", control_dir,
			      mailbox_list_get_hierarchy_sep(storage->list),
			      name);
	if (mkdir_parents(dir, CREATE_MODE) < 0 && errno != EEXIST) {
		mail_storage_set_critical(storage,
					  "mkdir(%s) failed: %m", dir);
		return -1;
	}

	return 0;
}

static int verify_inbox(struct mail_storage *storage,
			enum mailbox_open_flags *flags)
{
	const char *path;

	path = mailbox_list_get_path(storage->list, "INBOX",
				     MAILBOX_LIST_PATH_TYPE_MAILBOX);
	if (create_maildir(storage, path, TRUE) < 0)
		return -1;

	if (create_index_dir(storage, "INBOX") < 0)
		*flags |= MAILBOX_OPEN_NO_INDEX_FILES;
	if (create_control_dir(storage, "INBOX") < 0)
		return -1;
	return 0;
}

static bool maildir_is_recent(struct index_mailbox *ibox, uint32_t uid)
{
	struct maildir_mailbox *mbox = (struct maildir_mailbox *)ibox;

	return maildir_uidlist_is_recent(mbox->uidlist, uid);
}

static void maildir_lock_touch_timeout(struct maildir_mailbox *mbox)
{
	(void)maildir_uidlist_lock_touch(mbox->uidlist);
}

static struct mailbox *
maildir_open(struct maildir_storage *storage, const char *name,
	     enum mailbox_open_flags flags)
{
	struct maildir_mailbox *mbox;
	struct mail_index *index;
	const char *path, *index_dir, *control_dir;
	struct stat st;
	int shared;
	pool_t pool;

	path = mailbox_list_get_path(storage->storage.list, name,
				     MAILBOX_LIST_PATH_TYPE_MAILBOX);
	index_dir = mailbox_list_get_path(storage->storage.list, name,
					  MAILBOX_LIST_PATH_TYPE_INDEX);
	control_dir = mailbox_list_get_path(storage->storage.list, name,
					    MAILBOX_LIST_PATH_TYPE_CONTROL);

	if ((flags & MAILBOX_OPEN_NO_INDEX_FILES) != 0)
		index_dir = "";

	index = index_storage_alloc(index_dir, path,
				    MAILDIR_INDEX_PREFIX);

	/* for shared mailboxes get the create mode from the
	   permissions of dovecot-shared file. */
	shared = stat(t_strconcat(path, "/dovecot-shared", NULL), &st) == 0;
	if (shared)
		mail_index_set_permissions(index, st.st_mode & 0666, st.st_gid);

	pool = pool_alloconly_create("maildir mailbox", 1024+512);
	mbox = p_new(pool, struct maildir_mailbox, 1);
	mbox->ibox.box = maildir_mailbox;
	mbox->ibox.box.pool = pool;
	mbox->ibox.storage = &storage->storage;
	mbox->ibox.mail_vfuncs = &maildir_mail_vfuncs;
	mbox->ibox.is_recent = maildir_is_recent;
	mbox->ibox.index = index;

	mbox->storage = storage;
	mbox->path = p_strdup(pool, path);
	mbox->control_dir = p_strdup(pool, control_dir);

	mbox->uidlist = maildir_uidlist_init(mbox);
	mbox->keywords = maildir_keywords_init(mbox);

	if (!shared) {
		mbox->mail_create_mode = 0600;
		mbox->mail_create_gid = (gid_t)-1;
	} else {
		mbox->mail_create_mode = st.st_mode & 0666;
		mbox->mail_create_gid = st.st_gid;
		mbox->private_flags_mask = MAIL_SEEN;
	}

	if ((flags & MAILBOX_OPEN_KEEP_LOCKED) != 0) {
		if (maildir_uidlist_lock(mbox->uidlist) <= 0) {
			struct mailbox *box = &mbox->ibox.box;

			mailbox_close(&box);
			return NULL;
		}
		mbox->keep_lock_to = timeout_add(MAILDIR_LOCK_TOUCH_SECS * 1000,
						 maildir_lock_touch_timeout,
						 mbox);
	}

	index_storage_mailbox_init(&mbox->ibox, name, flags, FALSE);
	return &mbox->ibox.box;
}

static struct mailbox *
maildir_mailbox_open(struct mail_storage *_storage, const char *name,
		     struct istream *input, enum mailbox_open_flags flags)
{
	struct maildir_storage *storage = (struct maildir_storage *)_storage;
	const char *path;
	struct stat st;

	mail_storage_clear_error(_storage);

	if (input != NULL) {
		mail_storage_set_critical(_storage,
			"Maildir doesn't support streamed mailboxes");
		return NULL;
	}

	if (strcmp(name, "INBOX") == 0) {
		if (verify_inbox(_storage, &flags) < 0)
			return NULL;
		return maildir_open(storage, "INBOX", flags);
	}

	if (!mailbox_list_is_valid_existing_name(_storage->list, name)) {
		mail_storage_set_error(_storage, "Invalid mailbox name");
		return NULL;
	}

	path = mailbox_list_get_path(_storage->list, name,
				     MAILBOX_LIST_PATH_TYPE_MAILBOX);
	if (stat(path, &st) == 0) {
		/* exists - make sure the required directories are also there */
		if (create_maildir(_storage, path, TRUE) < 0 ||
		    create_control_dir(_storage, name) < 0)
			return NULL;

		if ((flags & MAILBOX_OPEN_NO_INDEX_FILES) == 0) {
			if (create_index_dir(_storage, name) < 0)
				flags |= MAILBOX_OPEN_NO_INDEX_FILES;
		}

		return maildir_open(storage, name, flags);
	} else if (errno == ENOENT) {
		mail_storage_set_error(_storage,
			MAILBOX_LIST_ERR_MAILBOX_NOT_FOUND, name);
		return NULL;
	} else {
		mail_storage_set_critical(_storage, "stat(%s) failed: %m",
					  path);
		return NULL;
	}
}

static int maildir_create_shared(struct mail_storage *storage,
				 const char *dir, mode_t mode, gid_t gid)
{
	const char *path;
	mode_t old_mask;
	int fd;

	/* add the execute bit if either read or write bit is set */
	if ((mode & 0600) != 0) mode |= 0100;
	if ((mode & 0060) != 0) mode |= 0010;
	if ((mode & 0006) != 0) mode |= 0001;

	old_mask = umask(0777 ^ mode);
	if (create_maildir(storage, dir, FALSE) < 0) {
		if (errno == EEXIST) {
			mail_storage_set_error(storage,
					       "Mailbox already exists");
		}
		umask(old_mask);
		return -1;
	}
	if (chown(dir, (uid_t)-1, gid) < 0) {
		mail_storage_set_critical(storage, "chown(%s) failed: %m", dir);
	}

	path = t_strconcat(dir, "/dovecot-shared", NULL);
	fd = open(path, O_WRONLY | O_CREAT, mode & 0666);
	umask(old_mask);

	if (fd == -1) {
		mail_storage_set_critical(storage, "open(%s) failed: %m", path);
		return -1;
	}

	if (fchown(fd, (uid_t)-1, gid) < 0) {
		mail_storage_set_critical(storage,
					  "fchown(%s) failed: %m", path);
	}
	(void)close(fd);
	return 0;
}

static int maildir_mailbox_create(struct mail_storage *_storage,
				  const char *name,
				  bool directory __attr_unused__)
{
	struct stat st;
	const char *path, *root_dir, *shared_path;
	int fd;

	mail_storage_clear_error(_storage);

	if (!mailbox_list_is_valid_create_name(_storage->list, name)) {
		mail_storage_set_error(_storage, "Invalid mailbox name");
		return -1;
	}

	path = mailbox_list_get_path(_storage->list, name,
				     MAILBOX_LIST_PATH_TYPE_MAILBOX);
	root_dir = mailbox_list_get_path(_storage->list, NULL,
					 MAILBOX_LIST_PATH_TYPE_MAILBOX);

	/* if dovecot-shared exists in the root dir, create the mailbox using
	   its permissions and gid, and copy the dovecot-shared inside it. */
	shared_path = t_strconcat(root_dir, "/dovecot-shared", NULL);
	if (stat(shared_path, &st) == 0) {
		return maildir_create_shared(_storage, path,
					     st.st_mode & 0666, st.st_gid);
	}

	if (create_maildir(_storage, path, FALSE) < 0) {
		if (errno == EEXIST) {
			mail_storage_set_error(_storage,
					       "Mailbox already exists");
		}
		return -1;
	}

	/* Maildir++ spec want that maildirfolder named file is created for
	   all subfolders. */
	path = t_strconcat(path, "/" MAILDIR_SUBFOLDER_FILENAME, NULL);
	fd = open(path, O_CREAT | O_WRONLY, CREATE_MODE & 0666);
	if (fd == -1)
		i_error("open(%s, O_CREAT) failed: %m", path);
	else
		(void)close(fd);
	return 0;
}

static const char *
maildir_get_unlink_dest(struct mailbox_list *list, const char *name)
{
	const char *root_dir;

	if ((list->flags & MAILBOX_LIST_FLAG_FULL_FS_ACCESS) != 0 &&
	    (*name == '/' || *name == '~'))
		return NULL;

	if (strcmp(mailbox_list_get_driver_name(list),
		   MAILDIR_PLUSPLUS_DRIVER_NAME) != 0) {
		/* Not maildir++ driver. Don't use this trick. */
		return NULL;
	}

	root_dir = mailbox_list_get_path(list, NULL,
					 MAILBOX_LIST_PATH_TYPE_DIR);
	return t_strdup_printf("%s/%c"MAILDIR_UNLINK_DIRNAME, root_dir,
			       mailbox_list_get_hierarchy_sep(list));
}

static int
maildir_delete_nonrecursive(struct mailbox_list *list, const char *path,
			    const char *name)
{
	DIR *dir;
	struct dirent *d;
	string_t *full_path;
	unsigned int dir_len;
	bool unlinked_something = FALSE;

	dir = opendir(path);
	if (dir == NULL) {
		if (errno == ENOENT) {
			mailbox_list_set_error(list, t_strdup_printf(
				MAILBOX_LIST_ERR_MAILBOX_NOT_FOUND, name));
		} else {
			mailbox_list_set_critical(list,
				"opendir(%s) failed: %m", path);
		}
		return -1;
	}

	full_path = t_str_new(256);
	str_append(full_path, path);
	str_append_c(full_path, '/');
	dir_len = str_len(full_path);

	errno = 0;
	while ((d = readdir(dir)) != NULL) {
		if (d->d_name[0] == '.') {
			/* skip . and .. */
			if (d->d_name[1] == '\0')
				continue;
			if (d->d_name[1] == '.' && d->d_name[2] == '\0')
				continue;
		}

		str_truncate(full_path, dir_len);
		str_append(full_path, d->d_name);

		if (maildir_is_internal_name(d->d_name)) {
			if (unlink_directory(str_c(full_path), TRUE) < 0) {
				mailbox_list_set_critical(list,
					"unlink_directory(%s) failed: %m",
					str_c(full_path));
			} else {
				unlinked_something = TRUE;
			}
			continue;
		}

		/* trying to unlink() a directory gives either EPERM or EISDIR
		   (non-POSIX). it doesn't really work anywhere in practise,
		   so don't bother stat()ing the file first */
		if (unlink(str_c(full_path)) == 0)
			unlinked_something = TRUE;
		else if (errno != ENOENT && errno != EISDIR && errno != EPERM) {
			mailbox_list_set_critical(list,
				"unlink_directory(%s) failed: %m",
				str_c(full_path));
		}
	}

	if (closedir(dir) < 0) {
		mailbox_list_set_critical(list, "closedir(%s) failed: %m",
					  path);
	}

	if (rmdir(path) == 0)
		unlinked_something = TRUE;
	else if (errno != ENOENT && errno != ENOTEMPTY) {
		mailbox_list_set_critical(list, "rmdir(%s) failed: %m", path);
		return -1;
	}

	if (!unlinked_something) {
		mailbox_list_set_error(list, t_strdup_printf(
			"Directory %s isn't empty, can't delete it.", name));
		return -1;
	}
	return 0;
}

static int
maildir_list_delete_mailbox(struct mailbox_list *list, const char *name)
{
	struct maildir_storage *storage = MAILDIR_LIST_CONTEXT(list);
	struct stat st;
	const char *src, *dest;
	int count;

	/* Make sure the indexes are closed before trying to delete the
	   directory that contains them. It can still fail with some NFS
	   implementations if indexes are opened by another session, but
	   that can't really be helped. */
	index_storage_destroy_unrefed();

	/* delete the index and control directories */
	if (storage->list_module_ctx.super.delete_mailbox(list, name) < 0)
		return -1;

	/* check if the mailbox actually exists */
	src = mailbox_list_get_path(list, name, MAILBOX_LIST_PATH_TYPE_MAILBOX);
	if (stat(src, &st) != 0 && errno == ENOENT) {
		mailbox_list_set_error(list, t_strdup_printf(
			MAILBOX_LIST_ERR_MAILBOX_NOT_FOUND, name));
		return -1;
	}

	dest = maildir_get_unlink_dest(list, name);
	if (dest == NULL) {
		/* delete the directory directly without any renaming */
		return maildir_delete_nonrecursive(list, src, name);
	}

	/* rename the .maildir into ..DOVECOT-TRASH which atomically
	   marks it as being deleted. If we die before deleting the
	   ..DOVECOT-TRASH directory, it gets deleted the next time
	   mailbox listing sees it. */
	count = 0;
	while (rename(src, dest) < 0 && count < 2) {
		if (errno == ENOENT) {
			/* it was just deleted under us by
			   another process */
			mailbox_list_set_error(list, t_strdup_printf(
				MAILBOX_LIST_ERR_MAILBOX_NOT_FOUND, name));
			return -1;
		}
		if (!EDESTDIREXISTS(errno)) {
			mailbox_list_set_critical(list,
				"rename(%s, %s) failed: %m", src, dest);
			return -1;
		}

		/* already existed, delete it and try again */
		if (unlink_directory(dest, TRUE) < 0) {
			mailbox_list_set_critical(list,
				"unlink_directory(%s) failed: %m", dest);
			return -1;
		}
		count++;
	}

	if (unlink_directory(dest, TRUE) < 0 && errno != ENOTEMPTY) {
		mailbox_list_set_critical(list,
			"unlink_directory(%s) failed: %m", dest);

		/* it's already renamed to ..dir, which means it's
		   deleted as far as the client is concerned. Report
		   success. */
	}
	return 0;
}

static int maildir_list_rename_mailbox(struct mailbox_list *list,
				       const char *oldname, const char *newname)
{
	struct maildir_storage *storage = MAILDIR_LIST_CONTEXT(list);
	const char *path1, *path2;

	if (strcmp(oldname, "INBOX") == 0) {
		/* INBOX often exists as the root ~/Maildir.
		   We can't rename it then. */
		path1 = mailbox_list_get_path(list, oldname,
					      MAILBOX_LIST_PATH_TYPE_MAILBOX);
		path2 = mailbox_list_get_path(list, NULL,
					      MAILBOX_LIST_PATH_TYPE_MAILBOX);
		if (strcmp(path1, path2) == 0) {
			mailbox_list_set_error(list,
				"Renaming INBOX isn't supported.");
			return -1;
		}
	}

	return storage->list_module_ctx.super.
		rename_mailbox(list, oldname, newname);
}

static int maildir_storage_close(struct mailbox *box)
{
	struct maildir_mailbox *mbox = (struct maildir_mailbox *)box;
	int ret = 0;

	/*FIXME:if (!maildir_try_flush_dirty_flags(ibox->index, TRUE)) {
		mail_storage_set_index_error(ibox);
		ret = -1;
	}*/

	if (mbox->keep_lock_to != NULL) {
		maildir_uidlist_unlock(mbox->uidlist);
		timeout_remove(&mbox->keep_lock_to);
	}

	maildir_keywords_deinit(mbox->keywords);
	maildir_uidlist_deinit(mbox->uidlist);
        index_storage_mailbox_free(box);
	return ret;
}

static void
maildir_notify_changes(struct mailbox *box, unsigned int min_interval,
		       mailbox_notify_callback_t *callback, void *context)
{
	struct maildir_mailbox *mbox = (struct maildir_mailbox *)box;

	mbox->ibox.min_notify_interval = min_interval;
	mbox->ibox.notify_callback = callback;
	mbox->ibox.notify_context = context;

	if (callback == NULL) {
		index_mailbox_check_remove_all(&mbox->ibox);
		return;
	}

	index_mailbox_check_add(&mbox->ibox,
		t_strconcat(mbox->path, "/new", NULL));
	index_mailbox_check_add(&mbox->ibox,
		t_strconcat(mbox->path, "/cur", NULL));
}

static int
maildir_list_iter_is_mailbox(struct mailbox_list_iterate_context *ctx
			     	__attr_unused__,
			     const char *dir, const char *fname,
			     enum mailbox_list_file_type type,
			     enum mailbox_info_flags *flags_r)
{
	struct stat st;
	const char *path;
	int ret;

	if (maildir_is_internal_name(fname)) {
		*flags_r = MAILBOX_NONEXISTENT;
		return 0;
	}

	switch (type) {
	case MAILBOX_LIST_FILE_TYPE_FILE:
	case MAILBOX_LIST_FILE_TYPE_OTHER:
		/* non-directories are not */
		*flags_r = MAILBOX_NOSELECT;
		return 0;

	case MAILBOX_LIST_FILE_TYPE_DIR:
	case MAILBOX_LIST_FILE_TYPE_UNKNOWN:
	case MAILBOX_LIST_FILE_TYPE_SYMLINK:
		break;
	}

	t_push();
	path = t_strdup_printf("%s/%s", dir, fname);
	if (stat(path, &st) == 0) {
		if (S_ISDIR(st.st_mode))
			ret = 1;
		else if (strncmp(fname, ".nfs", 4) == 0) {
			/* temporary NFS file */
			*flags_r = MAILBOX_NONEXISTENT;
			ret = 0;
		} else {
			*flags_r = MAILBOX_NOSELECT;
			ret = 0;
		}
	} else if (errno == ENOENT) {
		/* this was a directory. maybe it has children. */
		*flags_r = MAILBOX_NOSELECT;
		ret = 1;
	} else {
		*flags_r = MAILBOX_NOSELECT;
		ret = 0;
	}
	t_pop();
	return ret;
}

static int
maildirplusplus_iter_is_mailbox(struct mailbox_list_iterate_context *ctx,
				const char *dir, const char *fname,
				enum mailbox_list_file_type type,
				enum mailbox_info_flags *flags_r)
{
	struct maildir_storage *storage = MAILDIR_LIST_CONTEXT(ctx->list);
	struct mail_storage *_storage = &storage->storage;
	int ret;

	if (fname[1] == mailbox_list_get_hierarchy_sep(_storage->list) &&
	    strcmp(fname+2, MAILDIR_UNLINK_DIRNAME) == 0) {
		const char *path;
		struct stat st;

		/* this directory is in the middle of being deleted,
		   or the process trying to delete it had died.
		   delete it ourself if it's been there longer than
		   one hour. */
		t_push();
		path = t_strdup_printf("%s/%s", dir, fname);
		if (stat(path, &st) == 0 &&
		    st.st_mtime < ioloop_time - 3600)
			(void)unlink_directory(path, TRUE);
		t_pop();

		*flags_r = MAILBOX_NONEXISTENT;
		return 0;
	}

	switch (type) {
	case MAILBOX_LIST_FILE_TYPE_DIR:
		/* all directories are valid maildirs */
		return 1;

	case MAILBOX_LIST_FILE_TYPE_FILE:
	case MAILBOX_LIST_FILE_TYPE_OTHER:
		/* non-directories are not */
		*flags_r = MAILBOX_NOSELECT;
		return 0;

	case MAILBOX_LIST_FILE_TYPE_UNKNOWN:
	case MAILBOX_LIST_FILE_TYPE_SYMLINK:
		/* need to check with stat() to be sure */
		break;
	}

	/* Check files beginning with .nfs always because they may be
	   temporary files created by the kernel */
	if (storage->stat_dirs || strncmp(fname, ".nfs", 4) == 0) {
		const char *path;
		struct stat st;

		t_push();
		path = t_strdup_printf("%s/%s", dir, fname);
		if (stat(path, &st) == 0) {
			if (S_ISDIR(st.st_mode))
				ret = 1;
			else {
				if (strncmp(fname, ".nfs", 4) == 0)
					*flags_r = MAILBOX_NONEXISTENT;
				else
					*flags_r = MAILBOX_NOSELECT;
				ret = 0;
			}
		} else if (errno == ENOENT) {
			/* just deleted? */
			*flags_r = MAILBOX_NONEXISTENT;
			ret = 0;
		} else {
			*flags_r = MAILBOX_NOSELECT;
			ret = 0;
		}
		t_pop();
	} else {
		ret = 1;
	}
	return ret;
}

static void maildir_class_init(void)
{
	maildir_transaction_class_init();
}

static void maildir_class_deinit(void)
{
	maildir_transaction_class_deinit();
}

struct mail_storage maildir_storage = {
	MEMBER(name) MAILDIR_STORAGE_NAME,
	MEMBER(mailbox_is_file) FALSE,

	{
		maildir_class_init,
		maildir_class_deinit,
		maildir_create,
		maildir_free,
		maildir_autodetect,
		index_storage_set_callbacks,
		maildir_mailbox_open,
		maildir_mailbox_create,
		index_storage_get_last_error
	}
};

struct mailbox maildir_mailbox = {
	MEMBER(name) NULL, 
	MEMBER(storage) NULL, 

	{
		index_storage_is_readonly,
		index_storage_allow_new_keywords,
		maildir_storage_close,
		index_storage_get_status,
		maildir_storage_sync_init,
		index_mailbox_sync_next,
		index_mailbox_sync_deinit,
		maildir_notify_changes,
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
		index_storage_search_next_nonblock,
		index_storage_search_next_update_seq,
		maildir_save_init,
		maildir_save_continue,
		maildir_save_finish,
		maildir_save_cancel,
		maildir_copy,
		index_storage_is_inconsistent
	}
};
