/*
 * APOP (RFC-1460) authentication mechanism.
 *
 * Copyright (c) 2004 Andrey Panin <pazke@donpac.ru>
 *
 * This software is released under the MIT license.
 */

#include "common.h"
#include "mech.h"
#include "passdb.h"
#include "md5.h"
#include "buffer.h"
#include "auth-client-connection.h"
#include "auth-master-connection.h"

#include <stdio.h>
#include <unistd.h>

struct apop_auth_request {
	struct auth_request auth_request;

	pool_t pool;

	/* requested: */
	char *challenge;

	/* received: */
	unsigned char digest[16];
};

static bool verify_credentials(struct apop_auth_request *request,
			       const char *credentials)
{
	unsigned char digest[16];
	struct md5_context ctx;

	md5_init(&ctx);
	md5_update(&ctx, request->challenge, strlen(request->challenge));
	md5_update(&ctx, credentials, strlen(credentials));
	md5_final(&ctx, digest);

	return memcmp(digest, request->digest, 16) == 0;
}

static void
apop_credentials_callback(enum passdb_result result,
			  const char *credentials,
			  struct auth_request *auth_request)
{
	struct apop_auth_request *request =
		(struct apop_auth_request *)auth_request;

	switch (result) {
	case PASSDB_RESULT_OK:
		if (verify_credentials(request, credentials))
			auth_request_success(auth_request, NULL, 0);
		else
			auth_request_fail(auth_request);
		break;
	case PASSDB_RESULT_INTERNAL_FAILURE:
		auth_request_internal_failure(auth_request);
		break;
	default:
		auth_request_fail(auth_request);
		break;
	}
}

static void
mech_apop_auth_initial(struct auth_request *auth_request,
		       const unsigned char *data, size_t data_size)
{
	struct apop_auth_request *request =
		(struct apop_auth_request *)auth_request;
	const unsigned char *tmp, *end, *username = NULL;
	unsigned long pid, connect_uid, timestamp;
	const char *error;

	if (data_size == 0) {
		/* Should never happen */
		auth_request_log_info(auth_request, "apop",
				      "no initial respone");
		auth_request_fail(auth_request);
		return;
	}

	tmp = data;
	end = data + data_size;

	while (tmp != end && *tmp != '\0')
		tmp++;

	/* the challenge must begin with trusted unique ID. we trust only
	   ourself, so make sure it matches our connection specific UID
	   which we told to client in handshake. Also require a timestamp
	   which is later than this process's start time. */

	if (sscanf((const char *)data, "<%lx.%lx.%lx.",
		   &pid, &connect_uid, &timestamp) != 3 ||
	    connect_uid != auth_request->connect_uid ||
            pid != (unsigned long)getpid() ||
	    (time_t)timestamp < process_start_time) {
		auth_request_log_info(auth_request, "apop",
				      "invalid challenge");
		auth_request_fail(auth_request);
		return;
	}
	request->challenge = p_strdup(request->pool, (const char *)data);

	if (tmp != end) {
		username = ++tmp;
		while (tmp != end && *tmp != '\0')
			tmp++;
	}

	if (tmp + 1 + 16 != end) {
		/* Should never happen */
		auth_request_log_info(auth_request, "apop", "malformed data");
		auth_request_fail(auth_request);
		return;
	}
	tmp++;

	if (!auth_request_set_username(auth_request, (const char *)username,
				       &error)) {
		auth_request_log_info(auth_request, "apop", "%s", error);
		auth_request_fail(auth_request);
		return;
	}

	memcpy(request->digest, tmp, sizeof(request->digest));

	auth_request_lookup_credentials(auth_request,
					PASSDB_CREDENTIALS_PLAINTEXT,
					apop_credentials_callback);
}

static struct auth_request *mech_apop_auth_new(void)
{
	struct apop_auth_request *request;
	pool_t pool;

	pool = pool_alloconly_create("apop_auth_request", 1024);
	request = p_new(pool, struct apop_auth_request, 1);
	request->pool = pool;

	request->auth_request.pool = pool;
	return &request->auth_request;
}

const struct mech_module mech_apop = {
	"APOP",

	MEMBER(flags) MECH_SEC_PRIVATE | MECH_SEC_DICTIONARY | MECH_SEC_ACTIVE,

	MEMBER(passdb_need_plain) FALSE,
	MEMBER(passdb_need_credentials) TRUE,

	mech_apop_auth_new,
	mech_apop_auth_initial,
	NULL,
        mech_generic_auth_free
};
