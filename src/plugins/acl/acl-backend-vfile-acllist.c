/* Copyright (C) 2007 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "safe-mkstemp.h"
#include "istream.h"
#include "ostream.h"
#include "mail-namespace.h"
#include "mail-storage.h"
#include "acl-plugin.h"
#include "acl-cache.h"
#include "acl-backend-vfile.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

struct acl_mailbox_list_context_vfile {
	struct acl_mailbox_list_context ctx;

	unsigned int acllist_change_counter;
	unsigned int idx;
};

static void
acllist_clear(struct acl_backend_vfile *backend, uoff_t file_size)
{
	backend->acllist_change_counter++;
	if (backend->acllist_pool == NULL) {
		backend->acllist_pool =
			pool_alloconly_create("vfile acllist",
					      I_MAX(file_size / 2, 128));
		i_array_init(&backend->acllist, I_MAX(16, file_size / 60));
	} else {
		p_clear(backend->acllist_pool);
		array_clear(&backend->acllist);
	}
}

static int acl_backend_vfile_acllist_read(struct acl_backend_vfile *backend)
{
	struct acl_backend_vfile_acllist acllist;
	struct istream *input;
	struct stat st;
	const char *rootdir, *path, *line, *p;
	int fd, ret = 0;

	backend->acllist_last_check = ioloop_time;

	rootdir = mailbox_list_get_path(backend->backend.list, NULL,
					MAILBOX_LIST_PATH_TYPE_DIR);
	path = t_strdup_printf("%s/"ACLLIST_FILENAME, rootdir);

	if (backend->acllist_mtime != 0) {
		/* see if the file's mtime has changed */
		if (stat(path, &st) < 0) {
			if (errno == ENOENT)
				backend->acllist_mtime = 0;
			else
				i_error("stat(%s) failed: %m", path);
			return -1;
		}
		if (st.st_mtime == backend->acllist_mtime)
			return 0;
	}

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		if (errno == ENOENT) {
			backend->acllist_mtime = 0;
			return -1;
		}
		i_error("open(%s) failed: %m", path);
		return -1;
	}
	if (fstat(fd, &st) < 0) {
		i_error("fstat(%s) failed: %m", path);
		(void)close(fd);
		return -1;
	}
	backend->acllist_mtime = st.st_mtime;
	acllist_clear(backend, st.st_size);

	input = i_stream_create_file(fd, (size_t)-1, FALSE);
	while ((line = i_stream_read_next_line(input)) != NULL) {
		acllist.mtime = 0;
		for (p = line; *p >= '0' && *p <= '9'; p++)
			acllist.mtime = acllist.mtime * 10 + (*p - '0');

		if (p == line || *p != ' ' || p[1] == '\0') {
			i_error("Broken acllist file: %s", path);
			if (unlink(path) < 0 && errno != ENOENT)
				i_error("unlink(%s) failed: %m", path);
			return -1;
		}
		acllist.name = p_strdup(backend->acllist_pool, p + 1);
		array_append(&backend->acllist, &acllist, 1);
	}
	if (input->stream_errno != 0)
		ret = -1;
	i_stream_destroy(&input);

	if (close(fd) < 0)
		i_error("close(%s) failed: %m", path);
	return ret;
}

void acl_backend_vfile_acllist_refresh(struct acl_backend_vfile *backend)
{
	if (backend->acllist_last_check +
	    (time_t)backend->cache_secs > ioloop_time)
		return;

	if (acl_backend_vfile_acllist_read(backend) < 0) {
		acllist_clear(backend, 0);
		if (!backend->rebuilding_acllist)
			(void)acl_backend_vfile_acllist_rebuild(backend);
	}
}

static bool rights_has_lookup_changes(const struct acl_rights *rights)
{
	const char *const *p;

	if (rights->id_type == ACL_ID_OWNER) {
		/* ignore owner rights */
		return FALSE;
	}

	if (rights->rights == NULL)
		return FALSE;

	for (p = rights->rights; *p != NULL; p++) {
		if (strcmp(*p, MAIL_ACL_LOOKUP) == 0)
			return TRUE;
	}
	return FALSE;
}

static int
acllist_append(struct acl_backend_vfile *backend, struct ostream *output,
	       struct mail_storage *storage, const char *name)
{
	struct acl_object *aclobj;
	struct acl_object_list_iter *iter;
	struct acl_rights rights;
	struct acl_backend_vfile_acllist acllist;
	const char *line;
	int ret;

	acl_cache_flush(backend->backend.cache, name);
	aclobj = acl_object_init_from_name(&backend->backend, storage, name);

	iter = acl_object_list_init(aclobj);
	while ((ret = acl_object_list_next(iter, &rights)) > 0) {
		if (rights_has_lookup_changes(&rights))
			break;
	}
	acl_object_list_deinit(&iter);

	if (acl_backend_vfile_object_get_mtime(aclobj, &acllist.mtime) < 0)
		ret = -1;

	if (ret > 0) {
		acllist.name = p_strdup(backend->acllist_pool, name);
		array_append(&backend->acllist, &acllist, 1);

		t_push();
		line = t_strdup_printf("%s %s\n", dec2str(acllist.mtime), name);
		o_stream_send_str(output, line);
		t_pop();
	}
	acl_object_deinit(&aclobj);
	return ret < 0 ? -1 : 0;
}

int acl_backend_vfile_acllist_rebuild(struct acl_backend_vfile *backend)
{
	struct mailbox_list *list = backend->backend.list;
	struct mail_namespace *ns;
	struct mailbox_list_iterate_context *iter;
	const struct mailbox_info *info;
	const char *rootdir, *acllist_path;
	struct ostream *output;
	struct stat st;
	string_t *path;
	mode_t mode;
	uid_t uid;
	gid_t gid;
	int fd, ret;

	ret = mailbox_list_get_permissions(list, NULL, &mode, &uid, &gid);
	if (ret <= 0) {
		/* Return success if the whole root directory was lost */
		return ret;
	}

	path = t_str_new(256);
	rootdir = mailbox_list_get_path(list, NULL,
					MAILBOX_LIST_PATH_TYPE_DIR);
	str_printfa(path, "%s/%s", rootdir, mailbox_list_get_temp_prefix(list));

	/* Build it into a temporary file and rename() over. There's no need
	   to use locking, because even if multiple processes are rebuilding
	   the file at the same time the result should be the same. */
	fd = safe_mkstemp(path, mode, uid, gid);
	if (fd == -1)
		return -1;
	output = o_stream_create_file(fd, 0, FALSE);

	ret = 0;
	acllist_clear(backend, 0);
	ns = mailbox_list_get_namespace(list);

	backend->rebuilding_acllist = TRUE;
	iter = mailbox_list_iter_init(list, "*", MAILBOX_LIST_ITER_RAW_LIST |
				      MAILBOX_LIST_ITER_RETURN_NO_FLAGS);
	while ((info = mailbox_list_iter_next(iter)) != NULL) {
		if (acllist_append(backend, output, ns->storage,
				   info->name) < 0) {
			ret = -1;
			break;
		}
	}

	if (mailbox_list_iter_deinit(&iter) < 0)
		ret = -1;
	o_stream_destroy(&output);
	backend->rebuilding_acllist = FALSE;

	if (ret == 0) {
		if (fstat(fd, &st) < 0) {
			i_error("fstat(%s) failed: %m", str_c(path));
			ret = -1;
		}
	}
	if (close(fd) < 0) {
		i_error("close(%s) failed: %m", str_c(path));
		ret = -1;
	}

	if (ret == 0) {
		acllist_path = t_strdup_printf("%s/"ACLLIST_FILENAME, rootdir);
		if (rename(str_c(path), acllist_path) < 0) {
			i_error("rename(%s, %s) failed: %m",
				str_c(path), acllist_path);
			ret = -1;
		}
	}
	if (ret == 0) {
		backend->acllist_mtime = st.st_mtime;
		backend->acllist_last_check = ioloop_time;
	} else {
		acllist_clear(backend, 0);
		if (unlink(str_c(path)) < 0 && errno != ENOENT)
			i_error("unlink(%s) failed: %m", str_c(path));
	}
	return ret;
}

static const struct acl_backend_vfile_acllist *
acl_backend_vfile_acllist_find(struct acl_backend_vfile *backend,
			       const char *name)
{
	const struct acl_backend_vfile_acllist *acllist;
	unsigned int i, count;

	acllist = array_get(&backend->acllist, &count);
	for (i = 0; i < count; i++) {
		if (strcmp(acllist[i].name, name) == 0)
			return &acllist[i];
	}
	return NULL;
}

void acl_backend_vfile_acllist_verify(struct acl_backend_vfile *backend,
				      const char *name, time_t mtime)
{
	const struct acl_backend_vfile_acllist *acllist;

	acl_backend_vfile_acllist_refresh(backend);
	acllist = acl_backend_vfile_acllist_find(backend, name);
	if (acllist != NULL && acllist->mtime != mtime &&
	    !backend->rebuilding_acllist)
		(void)acl_backend_vfile_acllist_rebuild(backend);
}

struct acl_mailbox_list_context *
acl_backend_vfile_nonowner_iter_init(struct acl_backend *_backend)
{
	struct acl_backend_vfile *backend =
		(struct acl_backend_vfile *)_backend;
	struct acl_mailbox_list_context_vfile *ctx;

	acl_backend_vfile_acllist_refresh(backend);

	ctx = i_new(struct acl_mailbox_list_context_vfile, 1);
	ctx->ctx.backend = _backend;
	ctx->acllist_change_counter = backend->acllist_change_counter;
	return &ctx->ctx;
}

int acl_backend_vfile_nonowner_iter_next(struct acl_mailbox_list_context *_ctx,
					 const char **name_r)
{
	struct acl_mailbox_list_context_vfile *ctx =
		(struct acl_mailbox_list_context_vfile *)_ctx;
	struct acl_backend_vfile *backend =
		(struct acl_backend_vfile *)_ctx->backend;
	const struct acl_backend_vfile_acllist *acllist;
	unsigned int count;

	if (ctx->acllist_change_counter != backend->acllist_change_counter)
		return -1;

	acllist = array_get(&backend->acllist, &count);
	if (ctx->idx == count)
		return 0;

	*name_r = acllist[ctx->idx++].name;
	return 1;
}

void
acl_backend_vfile_nonowner_iter_deinit(struct acl_mailbox_list_context *ctx)
{
	i_free(ctx);
}
