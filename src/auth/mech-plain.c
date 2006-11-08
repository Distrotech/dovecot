/* Copyright (C) 2002 Timo Sirainen */

#include "common.h"
#include "safe-memset.h"
#include "mech.h"
#include "passdb.h"

static void verify_callback(enum passdb_result result,
			    struct auth_request *request)
{
	switch (result) {
	case PASSDB_RESULT_OK:
		auth_request_success(request, NULL, 0);
		break;
	case PASSDB_RESULT_INTERNAL_FAILURE:
		auth_request_internal_failure(request);
		break;
	default:
		auth_request_fail(request);
		break;
	}
}

static void
mech_plain_auth_continue(struct auth_request *request,
			 const unsigned char *data, size_t data_size)
{
	const char *authid, *authenid, *error;
	char *pass;
	size_t i, len;
	int count;

	/* authorization ID \0 authentication ID \0 pass.
	   we'll ignore authorization ID for now. */
	authid = (const char *) data;
	authenid = NULL; pass = "";

	count = 0;
	for (i = 0; i < data_size; i++) {
		if (data[i] == '\0') {
			if (++count == 1)
				authenid = (const char *) data + i+1;
			else {
				i++;
				len = data_size - i;
				pass = p_strndup(unsafe_data_stack_pool,
						 data+i, len);
				break;
			}
		}
	}

	if (count != 2) {
		/* invalid input */
		auth_request_log_info(request, "plain", "invalid input");
		auth_request_fail(request);
	} else if (!auth_request_set_username(request, authenid, &error)) {
                /* invalid username */
                auth_request_log_info(request, "plain", "%s", error);
                auth_request_fail(request);
        } else if (*authid != '\0' &&
                   !auth_request_set_login_username(request, authid, &error)) {
                /* invalid login username */
                auth_request_log_info(request, "plain",
                                      "login user: %s", error);
                auth_request_fail(request);
        } else {
                auth_request_verify_plain(request, pass,
                                          verify_callback);
	}

        /* make sure it's cleared */
        safe_memset(pass, 0, strlen(pass));
}

static struct auth_request *mech_plain_auth_new(void)
{
        struct auth_request *request;
	pool_t pool;

	pool = pool_alloconly_create("plain_auth_request", 1024);
	request = p_new(pool, struct auth_request, 1);
	request->pool = pool;
	return request;
}

struct mech_module mech_plain = {
	"PLAIN",

	MEMBER(flags) MECH_SEC_PLAINTEXT,

	MEMBER(passdb_need_plain) TRUE,
	MEMBER(passdb_need_credentials) FALSE,
	MEMBER(passdb_need_set_credentials) FALSE,

	mech_plain_auth_new,
	mech_generic_auth_initial,
	mech_plain_auth_continue,
	mech_generic_auth_free
};
