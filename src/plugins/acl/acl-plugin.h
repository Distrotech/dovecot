#ifndef __ACL_PLUGIN_H
#define __ACL_PLUGIN_H

#include "mail-storage-private.h"

#define ACL_CONTEXT(obj) \
	*((void **)array_idx_modifiable(&(obj)->module_contexts, \
					acl_storage_module_id))

enum acl_storage_rights {
	ACL_STORAGE_RIGHT_LOOKUP,
	ACL_STORAGE_RIGHT_READ,
	ACL_STORAGE_RIGHT_WRITE,
	ACL_STORAGE_RIGHT_WRITE_SEEN,
	ACL_STORAGE_RIGHT_WRITE_DELETED,
	ACL_STORAGE_RIGHT_INSERT,
	ACL_STORAGE_RIGHT_EXPUNGE,
	ACL_STORAGE_RIGHT_CREATE,
	ACL_STORAGE_RIGHT_DELETE,
	ACL_STORAGE_RIGHT_ADMIN,

	ACL_STORAGE_RIGHT_COUNT
};

struct acl_mail_storage {
	struct mail_storage_vfuncs super;
	struct acl_backend *backend;
	unsigned int acl_storage_right_idx[ACL_STORAGE_RIGHT_COUNT];
};

extern void (*acl_next_hook_mail_storage_created)
	(struct mail_storage *storage);
extern void (*acl_next_hook_mailbox_list_created)(struct mailbox_list *list);
extern unsigned int acl_storage_module_id;

void acl_mail_storage_created(struct mail_storage *storage);
void acl_mailbox_list_created(struct mailbox_list *list);

struct mailbox *acl_mailbox_open_box(struct mailbox *box);

const char *
acl_storage_get_parent_mailbox_name(struct mail_storage *storage,
				    const char *name);
int acl_storage_have_right(struct mail_storage *storage, const char *name,
			   unsigned int acl_storage_right_idx, bool *can_see_r);
void acl_mailbox_list_set_storage(struct mail_storage *storage);

void acl_plugin_init(void);
void acl_plugin_deinit(void);

#endif
