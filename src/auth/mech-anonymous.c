/* Copyright (C) 2002-2004 Timo Sirainen */

#include "common.h"
#include "mech.h"

static void
mech_anonymous_auth_continue(struct auth_request *request,
			     const unsigned char *data, size_t data_size)
{
	i_assert(request->auth->anonymous_username != NULL);

	if (request->auth->verbose) {
		/* temporarily set the user to the one that was given,
		   so that the log message goes right */
		request->user =
			p_strndup(pool_datastack_create(), data, data_size);
		auth_request_log_info(request, "anonymous", "login");
	}

	request->user = p_strdup(request->pool,
				 request->auth->anonymous_username);

	auth_request_success(request, NULL, 0);
}

static void
mech_anonymous_auth_initial(struct auth_request *request,
			    const unsigned char *data, size_t data_size)
{
	if (data_size == 0) {
		request->callback(request, AUTH_CLIENT_RESULT_CONTINUE,
				  NULL, 0);
	} else {
		mech_anonymous_auth_continue(request, data, data_size);
	}
}

static void
mech_anonymous_auth_free(struct auth_request *request)
{
	pool_unref(request->pool);
}

static struct auth_request *mech_anonymous_auth_new(void)
{
        struct auth_request *request;
	pool_t pool;

	pool = pool_alloconly_create("anonymous_auth_request", 256);
	request = p_new(pool, struct auth_request, 1);
	request->pool = pool;
	return request;
}

struct mech_module mech_anonymous = {
	"ANONYMOUS",

	MEMBER(flags) MECH_SEC_ANONYMOUS,

	MEMBER(passdb_need_plain) FALSE,
	MEMBER(passdb_need_credentials) FALSE,

	mech_anonymous_auth_new,
	mech_anonymous_auth_initial,
	mech_anonymous_auth_continue,
        mech_anonymous_auth_free
};
