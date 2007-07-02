/* Copyright (C) 2006 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "imap-match.h"
#include "mailbox-tree.h"
#include "mail-namespace.h"
#include "mailbox-list-private.h"
#include "acl-cache.h"
#include "acl-api-private.h"
#include "acl-plugin.h"

#include <stdlib.h>

#define ACL_LIST_CONTEXT(obj) \
	MODULE_CONTEXT(obj, acl_mailbox_list_module)

#define MAILBOX_FLAG_MATCHED 0x40000000

struct acl_mailbox_list {
	union mailbox_list_module_context module_ctx;

	struct acl_storage_rights_context rights;
};

struct acl_mailbox_list_iterate_context {
	struct mailbox_list_iterate_context ctx;
	struct mailbox_list_iterate_context *super_ctx;

	struct mailbox_tree_context *tree;
	struct mailbox_tree_iterate_context *tree_iter;
	struct mailbox_info info;
};

static MODULE_CONTEXT_DEFINE_INIT(acl_mailbox_list_module,
				  &mailbox_list_module_register);

struct acl_backend *acl_mailbox_list_get_backend(struct mailbox_list *list)
{
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(list);

	return alist->rights.backend;
}

const char *acl_mailbox_list_get_parent_mailbox_name(struct mailbox_list *list,
						     const char *name)
{
	const char *p;
	char sep;

	sep = mailbox_list_get_hierarchy_sep(list);
	p = strrchr(name, sep);
	return p == NULL ? "" : t_strdup_until(name, p);
}

static int
acl_mailbox_list_have_right(struct acl_mailbox_list *alist, const char *name,
			    unsigned int acl_storage_right_idx, bool *can_see_r)
{
	return acl_storage_rights_ctx_have_right(&alist->rights, name,
						 acl_storage_right_idx,
						 can_see_r);
}

static bool
acl_mailbox_try_list_fast(struct acl_mailbox_list_iterate_context *ctx,
			  const char *mask)
{
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(ctx->ctx.list);
	struct acl_backend *backend = alist->rights.backend;
	const unsigned int *idxp =
		alist->rights.acl_storage_right_idx + ACL_STORAGE_RIGHT_LOOKUP;
	const struct acl_mask *acl_mask;
	struct acl_mailbox_list_context *nonowner_list_ctx;
	struct imap_match_glob *glob;
	struct mailbox_node *node;
	const char *name;
	char sep;
	int try, ret;
	bool created;

	if ((ctx->ctx.flags & MAILBOX_LIST_ITER_RAW_LIST) != 0)
		return FALSE;

	if (acl_backend_get_default_rights(backend, &acl_mask) < 0 ||
	    acl_cache_mask_isset(acl_mask, *idxp))
		return FALSE;

	/* default is to not list mailboxes. we can optimize this. */
	t_push();
	sep = mailbox_list_get_hierarchy_sep(ctx->ctx.list);
	glob = imap_match_init(pool_datastack_create(), mask, TRUE, sep);

	for (try = 0; try < 2; try++) {
		nonowner_list_ctx =
			acl_backend_nonowner_lookups_iter_init(backend);
		ctx->tree = mailbox_tree_init(sep);

		while ((ret = acl_backend_nonowner_lookups_iter_next(
					nonowner_list_ctx, &name)) > 0) {
			switch (imap_match(glob, name)) {
			case IMAP_MATCH_YES:
				node = mailbox_tree_get(ctx->tree, name,
							&created);
				if (created)
					node->flags |= MAILBOX_NOCHILDREN;
				node->flags |= MAILBOX_FLAG_MATCHED;
				node->flags &= ~MAILBOX_NONEXISTENT;
				break;
			case IMAP_MATCH_PARENT:
				node = mailbox_tree_get(ctx->tree, name,
							&created);
				if (created)
					node->flags |= MAILBOX_NONEXISTENT;
				node->flags |= MAILBOX_FLAG_MATCHED |
					MAILBOX_CHILDREN;
				node->flags &= ~MAILBOX_NOCHILDREN;
				break;
			default:
				break;
			}
		}
		if (ret == 0)
			break;

		/* try again */
		mailbox_tree_deinit(&ctx->tree);
		acl_backend_nonowner_lookups_iter_deinit(&nonowner_list_ctx);
	}
	t_pop();
	if (ret < 0)
		return FALSE;

	ctx->tree_iter = mailbox_tree_iterate_init(ctx->tree, NULL,
						   MAILBOX_FLAG_MATCHED);
	return TRUE;
}

static struct mailbox_list_iterate_context *
acl_mailbox_list_iter_init(struct mailbox_list *list, const char *mask,
			   enum mailbox_list_iter_flags flags)
{
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(list);
	struct acl_mailbox_list_iterate_context *ctx;

	ctx = i_new(struct acl_mailbox_list_iterate_context, 1);
	ctx->ctx.list = list;
	ctx->ctx.flags = flags;

	if (!acl_mailbox_try_list_fast(ctx, mask)) {
		ctx->super_ctx =
			alist->module_ctx.super.iter_init(list, mask, flags);
	}
	return &ctx->ctx;
}

static const struct mailbox_info *
acl_mailbox_list_iter_next(struct mailbox_list_iterate_context *_ctx)
{
	struct acl_mailbox_list_iterate_context *ctx =
		(struct acl_mailbox_list_iterate_context *)_ctx;
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(_ctx->list);
	const struct mailbox_info *info;
	struct mailbox_node *node;
	int ret;

	for (;;) {
		if (ctx->tree_iter != NULL) {
			node = mailbox_tree_iterate_next(ctx->tree_iter,
							 &ctx->info.name);
			if (node == NULL)
				return NULL;
			ctx->info.flags = node->flags;
			info = &ctx->info;
		} else {
			info = alist->module_ctx.super.
				iter_next(ctx->super_ctx);
			if (info == NULL)
				return NULL;
		}

		if ((ctx->ctx.flags & MAILBOX_LIST_ITER_RAW_LIST) != 0) {
			/* skip ACL checks. */
			return info;
		}

		ret = acl_mailbox_list_have_right(alist, info->name,
						  ACL_STORAGE_RIGHT_LOOKUP,
						  NULL);
		if (ret > 0)
			return info;
		if (ret < 0) {
			ctx->ctx.failed = TRUE;
			return NULL;
		}

		/* no permission to see this mailbox */
		if ((ctx->info.flags & MAILBOX_SUBSCRIBED) != 0) {
			/* it's subscribed, show it as non-existent */
			if (info != &ctx->info) {
				ctx->info = *info;
				info = &ctx->info;
			}
			ctx->info.flags = MAILBOX_NONEXISTENT |
				MAILBOX_SUBSCRIBED;
			return info;
		}

		/* skip to next one */
	}
}

static int
acl_mailbox_list_iter_deinit(struct mailbox_list_iterate_context *_ctx)
{
	struct acl_mailbox_list_iterate_context *ctx =
		(struct acl_mailbox_list_iterate_context *)_ctx;
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(_ctx->list);
	int ret = ctx->ctx.failed ? -1 : 0;

	if (ctx->super_ctx != NULL) {
		if (alist->module_ctx.super.iter_deinit(ctx->super_ctx) < 0)
			ret = -1;
	}
	if (ctx->tree_iter != NULL)
		mailbox_tree_iterate_deinit(&ctx->tree_iter);

	i_free(ctx);
	return ret;
}

static int acl_get_mailbox_name_status(struct mailbox_list *list,
				       const char *name,
				       enum mailbox_name_status *status)
{
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(list);
	const char *parent;
	int ret;

	ret = acl_mailbox_list_have_right(alist, name, ACL_STORAGE_RIGHT_LOOKUP,
					  NULL);
	if (ret < 0)
		return -1;

	if (alist->module_ctx.super.get_mailbox_name_status(list, name,
							    status) < 0)
		return -1;
	if (ret > 0)
		return 0;

	/* we shouldn't reveal this mailbox's existance */
	switch (*status) {
	case MAILBOX_NAME_EXISTS:
		*status = MAILBOX_NAME_VALID;
		break;
	case MAILBOX_NAME_VALID:
	case MAILBOX_NAME_INVALID:
		break;
	case MAILBOX_NAME_NOINFERIORS:
		/* have to check if we are allowed to see the parent */
		t_push();
		parent = acl_mailbox_list_get_parent_mailbox_name(list, name);
		ret = acl_mailbox_list_have_right(alist, parent,
						  ACL_STORAGE_RIGHT_LOOKUP,
						  NULL);
		t_pop();

		if (ret < 0)
			return -1;
		if (ret == 0) {
			/* no permission to see the parent */
			*status = MAILBOX_NAME_VALID;
		}
		break;
	}
	return 0;
}

static int
acl_mailbox_list_delete(struct mailbox_list *list, const char *name)
{
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(list);
	bool can_see;
	int ret;

	ret = acl_mailbox_list_have_right(alist, name, ACL_STORAGE_RIGHT_DELETE,
					  &can_see);
	if (ret <= 0) {
		if (ret < 0)
			return -1;
		if (can_see) {
			mailbox_list_set_error(list, MAIL_ERROR_PERM,
					       MAIL_ERRSTR_NO_PERMISSION);
		} else {
			mailbox_list_set_error(list, MAIL_ERROR_NOTFOUND,
				T_MAIL_ERR_MAILBOX_NOT_FOUND(name));
		}
		return -1;
	}

	return alist->module_ctx.super.delete_mailbox(list, name);
}

static int
acl_mailbox_list_rename(struct mailbox_list *list,
			const char *oldname, const char *newname)
{
	struct acl_mailbox_list *alist = ACL_LIST_CONTEXT(list);
	bool can_see;
	int ret;

	/* renaming requires rights to delete the old mailbox */
	ret = acl_mailbox_list_have_right(alist, oldname,
					  ACL_STORAGE_RIGHT_DELETE, &can_see);
	if (ret <= 0) {
		if (ret < 0)
			return -1;
		if (can_see) {
			mailbox_list_set_error(list, MAIL_ERROR_PERM,
					       MAIL_ERRSTR_NO_PERMISSION);
		} else {
			mailbox_list_set_error(list, MAIL_ERROR_NOTFOUND,
				T_MAIL_ERR_MAILBOX_NOT_FOUND(oldname));
		}
		return 0;
	}

	/* and create the new one under the parent mailbox */
	t_push();
	ret = acl_mailbox_list_have_right(alist,
		acl_mailbox_list_get_parent_mailbox_name(list, newname),
		ACL_STORAGE_RIGHT_CREATE, NULL);
	t_pop();

	if (ret <= 0) {
		if (ret == 0) {
			/* Note that if the mailbox didn't have LOOKUP
			   permission, this not reveals to user the mailbox's
			   existence. Can't help it. */
			mailbox_list_set_error(list, MAIL_ERROR_PERM,
					       MAIL_ERRSTR_NO_PERMISSION);
		}
		return -1;
	}

	return alist->module_ctx.super.rename_mailbox(list, oldname, newname);
}

void acl_mailbox_list_created(struct mailbox_list *list)
{
	struct acl_mailbox_list *alist;
	struct acl_backend *backend;
	struct mail_namespace *ns;
	enum mailbox_list_flags flags;
	const char *acl_env, *current_username, *owner_username;
	bool owner = TRUE;

	if (acl_next_hook_mailbox_list_created != NULL)
		acl_next_hook_mailbox_list_created(list);

	acl_env = getenv("ACL");
	i_assert(acl_env != NULL);

	owner_username = getenv("USER");
	if (owner_username == NULL)
		i_fatal("ACL: USER environment not set");

	current_username = getenv("MASTER_USER");
	if (current_username == NULL)
		current_username = owner_username;
	else
		owner = strcmp(current_username, owner_username) == 0;

	/* We don't care about the username for non-private mailboxes.
	   It's used only when checking if we're the mailbox owner. We never
	   are for shared/public mailboxes. */
	ns = mailbox_list_get_namespace(list);
	if (ns->type != NAMESPACE_PRIVATE)
		owner = FALSE;

	/* FIXME: set groups */
	backend = acl_backend_init(acl_env, list, current_username,
				   getenv("ACL_GROUPS") == NULL ? NULL :
				   t_strsplit(getenv("ACL_GROUPS"), ","),
				   owner);
	if (backend == NULL)
		i_fatal("ACL backend initialization failed");

	flags = mailbox_list_get_flags(list);
	if ((flags & MAILBOX_LIST_FLAG_FULL_FS_ACCESS) != 0) {
		/* not necessarily, but safer to do this for now.. */
		i_fatal("mail_full_filesystem_access=yes is "
			"incompatible with ACLs");
	}

	alist = p_new(list->pool, struct acl_mailbox_list, 1);
	alist->module_ctx.super = list->v;
	list->v.iter_init = acl_mailbox_list_iter_init;
	list->v.iter_next = acl_mailbox_list_iter_next;
	list->v.iter_deinit = acl_mailbox_list_iter_deinit;
	list->v.get_mailbox_name_status = acl_get_mailbox_name_status;
	list->v.delete_mailbox = acl_mailbox_list_delete;
	list->v.rename_mailbox = acl_mailbox_list_rename;

	acl_storage_rights_ctx_init(&alist->rights, backend);

	MODULE_CONTEXT_SET(list, acl_mailbox_list_module, alist);
}
