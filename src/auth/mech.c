/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "ioloop.h"
#include "mech.h"
#include "str.h"
#include "passdb.h"

#include <stdlib.h>

static struct mech_module_list *mech_modules;

void mech_register_module(struct mech_module *module)
{
	struct mech_module_list *list;

	list = i_new(struct mech_module_list, 1);
	list->module = *module;

	list->next = mech_modules;
	mech_modules = list;
}

void mech_unregister_module(struct mech_module *module)
{
	struct mech_module_list **pos, *list;

	for (pos = &mech_modules; *pos != NULL; pos = &(*pos)->next) {
		if (strcmp((*pos)->module.mech_name, module->mech_name) == 0) {
			list = *pos;
			*pos = (*pos)->next;
			i_free(list);
			break;
		}
	}
}

struct mech_module *mech_module_find(const char *name)
{
	struct mech_module_list *list;

	for (list = mech_modules; list != NULL; list = list->next) {
		if (strcasecmp(list->module.mech_name, name) == 0)
			return &list->module;
	}
	return NULL;
}

void mech_generic_auth_initial(struct auth_request *request,
			       const unsigned char *data, size_t data_size)
{
	if (data_size == 0) {
		request->callback(request, AUTH_CLIENT_RESULT_CONTINUE,
				  NULL, 0);
	} else {
		request->mech->auth_continue(request, data, data_size);
	}
}

void mech_generic_auth_free(struct auth_request *request)
{
	pool_unref(request->pool);
}

extern struct mech_module mech_plain;
extern struct mech_module mech_login;
extern struct mech_module mech_apop;
extern struct mech_module mech_cram_md5;
extern struct mech_module mech_digest_md5;
extern struct mech_module mech_ntlm;
extern struct mech_module mech_otp;
extern struct mech_module mech_skey;
extern struct mech_module mech_rpa;
extern struct mech_module mech_anonymous;
#ifdef HAVE_GSSAPI
extern struct mech_module mech_gssapi;
#endif

void mech_init(void)
{
	mech_register_module(&mech_plain);
	mech_register_module(&mech_login);
	mech_register_module(&mech_apop);
	mech_register_module(&mech_cram_md5);
	mech_register_module(&mech_digest_md5);
	mech_register_module(&mech_ntlm);
	mech_register_module(&mech_otp);
	mech_register_module(&mech_skey);
	mech_register_module(&mech_rpa);
	mech_register_module(&mech_anonymous);
#ifdef HAVE_GSSAPI
	mech_register_module(&mech_gssapi);
#endif
}

void mech_deinit(void)
{
	mech_unregister_module(&mech_plain);
	mech_unregister_module(&mech_login);
	mech_unregister_module(&mech_apop);
	mech_unregister_module(&mech_cram_md5);
	mech_unregister_module(&mech_digest_md5);
	mech_unregister_module(&mech_ntlm);
	mech_unregister_module(&mech_otp);
	mech_unregister_module(&mech_skey);
	mech_unregister_module(&mech_rpa);
	mech_unregister_module(&mech_anonymous);
#ifdef HAVE_GSSAPI
	mech_unregister_module(&mech_gssapi);
#endif
}
