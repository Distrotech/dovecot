/*
   Based on auth_pam.c from popa3d by Solar Designer <solar@openwall.com>.

   You're allowed to do whatever you like with this software (including
   re-distribution in source and/or binary form, with or without
   modification), provided that credit is given where it is due and any
   modified versions are marked as such.  There's absolutely no warranty.
*/

#include "common.h"

#ifdef PASSDB_PAM

#include "lib-signals.h"
#include "str.h"
#include "var-expand.h"
#include "network.h"
#include "passdb.h"
#include "safe-memset.h"
#include "auth-cache.h"

#include <stdlib.h>

#ifdef HAVE_SECURITY_PAM_APPL_H
#  include <security/pam_appl.h>
#elif defined(HAVE_PAM_PAM_APPL_H)
#  include <pam/pam_appl.h>
#endif

#if !defined(_SECURITY_PAM_APPL_H) && !defined(LINUX_PAM) && !defined(_OPENPAM)
/* Sun's PAM doesn't use const. we use a bit dirty hack to check it.
   Originally it was just __sun__ check, but HP/UX also uses Sun's PAM
   so I thought this might work better. */
#  define linux_const
#else
#  define linux_const			const
#endif
typedef linux_const void *pam_item_t;

struct pam_passdb_module {
	struct passdb_module module;

	const char *service_name, *pam_cache_key;

	unsigned int pam_setcred:1;
	unsigned int pam_session:1;
	unsigned int failure_show_msg:1;
};

struct pam_conv_context {
	struct auth_request *request;
	const char *pass;
	const char *failure_msg;
};

static int
pam_userpass_conv(int num_msg, linux_const struct pam_message **msg,
		  struct pam_response **resp_r, void *appdata_ptr)
{
	/* @UNSAFE */
	struct pam_conv_context *ctx = appdata_ptr;
	struct passdb_module *_passdb = ctx->request->passdb->passdb;
	struct pam_passdb_module *passdb = (struct pam_passdb_module *)_passdb;
	struct pam_response *resp;
	char *string;
	int i;

	*resp_r = NULL;

	resp = calloc(num_msg, sizeof(struct pam_response));
	if (resp == NULL)
		i_fatal_status(FATAL_OUTOFMEM, "Out of memory");

	for (i = 0; i < num_msg; i++) {
		auth_request_log_debug(ctx->request, "pam",
				       "#%d/%d style=%d msg=%s", i+1, num_msg,
				       msg[i]->msg_style,
				       msg[i]->msg != NULL ? msg[i]->msg : "");
		switch (msg[i]->msg_style) {
		case PAM_PROMPT_ECHO_ON:
			/* Assume we're asking for user. We might not ever
			   get here because PAM already knows the user. */
			string = strdup(ctx->request->user);
			if (string == NULL)
				i_fatal_status(FATAL_OUTOFMEM, "Out of memory");
			break;
		case PAM_PROMPT_ECHO_OFF:
			/* Assume we're asking for password */
			if (passdb->failure_show_msg)
				ctx->failure_msg = t_strdup(msg[i]->msg);
			string = strdup(ctx->pass);
			if (string == NULL)
				i_fatal_status(FATAL_OUTOFMEM, "Out of memory");
			break;
		case PAM_ERROR_MSG:
		case PAM_TEXT_INFO:
			string = NULL;
			break;
		default:
			while (--i >= 0) {
				if (resp[i].resp != NULL) {
					safe_memset(resp[i].resp, 0,
						    strlen(resp[i].resp));
					free(resp[i].resp);
				}
			}

			free(resp);
			return PAM_CONV_ERR;
		}

		resp[i].resp_retcode = PAM_SUCCESS;
		resp[i].resp = string;
	}

	*resp_r = resp;
	return PAM_SUCCESS;
}

static int try_pam_auth(struct auth_request *request, pam_handle_t *pamh)
{
        struct passdb_module *_module = request->passdb->passdb;
        struct pam_passdb_module *module = (struct pam_passdb_module *)_module;
	pam_item_t item;
	int status;

	if ((status = pam_authenticate(pamh, 0)) != PAM_SUCCESS) {
		auth_request_log_error(request, "pam",
				       "pam_authenticate() failed: %s",
				       pam_strerror(pamh, status));
		return status;
	}

#ifdef HAVE_PAM_SETCRED
	if (module->pam_setcred) {
		if ((status = pam_setcred(pamh, PAM_ESTABLISH_CRED)) !=
		    PAM_SUCCESS) {
			auth_request_log_error(request, "pam",
					       "pam_setcred() failed: %s",
					       pam_strerror(pamh, status));
			return status;
		}
	}
#endif

	if ((status = pam_acct_mgmt(pamh, 0)) != PAM_SUCCESS) {
		auth_request_log_error(request, "pam",
				       "pam_acct_mgmt() failed: %s",
				       pam_strerror(pamh, status));
		return status;
	}

	if (module->pam_session) {
	        if ((status = pam_open_session(pamh, 0)) != PAM_SUCCESS) {
			auth_request_log_error(request, "pam",
					       "pam_open_session() failed: %s",
					       pam_strerror(pamh, status));
	                return status;
	        }

	        if ((status = pam_close_session(pamh, 0)) != PAM_SUCCESS) {
			auth_request_log_error(request, "pam",
					       "pam_close_session() failed: %s",
					       pam_strerror(pamh, status));
			return status;
	        }
	}

	status = pam_get_item(pamh, PAM_USER, &item);
	if (status != PAM_SUCCESS) {
		auth_request_log_error(request, "pam",
				       "pam_get_item(PAM_USER) failed: %s",
				       pam_strerror(pamh, status));
		return status;
	}
	auth_request_set_field(request, "user", item, NULL);
	return PAM_SUCCESS;
}

static void set_pam_items(struct auth_request *request, pam_handle_t *pamh)
{
	const char *host;

	/* These shouldn't fail, and we don't really care if they do. */
	host = net_ip2addr(&request->remote_ip);
	if (host != NULL)
		(void)pam_set_item(pamh, PAM_RHOST, host);
	(void)pam_set_item(pamh, PAM_RUSER, request->user);
	/* TTY is needed by eg. pam_access module */
	(void)pam_set_item(pamh, PAM_TTY, "dovecot");
}

static enum passdb_result 
pam_verify_plain_call(struct auth_request *request, const char *service,
		      const char *password)
{
	pam_handle_t *pamh;
	struct pam_conv_context ctx;
	struct pam_conv conv;
	enum passdb_result result;
	int status, status2;

	conv.conv = pam_userpass_conv;
	conv.appdata_ptr = &ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.request = request;
	ctx.pass = password;

	status = pam_start(service, request->user, &conv, &pamh);
	if (status != PAM_SUCCESS) {
		auth_request_log_error(request, "pam", "pam_start() failed: %s",
				       pam_strerror(pamh, status));
		return PASSDB_RESULT_INTERNAL_FAILURE;
	}

	set_pam_items(request, pamh);
	status = try_pam_auth(request, pamh);
	if ((status2 = pam_end(pamh, status)) != PAM_SUCCESS) {
		auth_request_log_error(request, "pam", "pam_end() failed: %s",
				       pam_strerror(pamh, status2));
		return PASSDB_RESULT_INTERNAL_FAILURE;
	}

	switch (status) {
	case PAM_SUCCESS:
		result = PASSDB_RESULT_OK;
		break;
	case PAM_USER_UNKNOWN:
		result = PASSDB_RESULT_USER_UNKNOWN;
		break;
	case PAM_NEW_AUTHTOK_REQD:
	case PAM_ACCT_EXPIRED:
		result = PASSDB_RESULT_PASS_EXPIRED;
		break;
	default:
		result = PASSDB_RESULT_PASSWORD_MISMATCH;
		break;
	}

	if (result != PASSDB_RESULT_OK && ctx.failure_msg != NULL) {
		auth_request_set_field(request, "reason",
				       ctx.failure_msg, NULL);
	}
	return result;
}

static void
pam_verify_plain(struct auth_request *request, const char *password,
		 verify_plain_callback_t *callback)
{
        struct passdb_module *_module = request->passdb->passdb;
        struct pam_passdb_module *module = (struct pam_passdb_module *)_module;
	enum passdb_result result;
	string_t *expanded_service;
	const char *service;

	expanded_service = t_str_new(64);
	var_expand(expanded_service, module->service_name,
		   auth_request_get_var_expand_table(request, NULL));
	service = str_c(expanded_service);

	auth_request_log_debug(request, "pam", "lookup service=%s", service);

	result = pam_verify_plain_call(request, service, password);
	callback(result, request);
}

static struct passdb_module *
pam_preinit(struct auth_passdb *auth_passdb, const char *args)
{
	struct pam_passdb_module *module;
	const char *const *t_args;
	int i;

	module = p_new(auth_passdb->auth->pool, struct pam_passdb_module, 1);
	module->service_name = "dovecot";
	/* we're caching the password by using directly the plaintext password
	   given by the auth mechanism */
	module->module.default_pass_scheme = "PLAIN";
	module->module.blocking = TRUE;

	t_args = t_strsplit_spaces(args, " ");
	for(i = 0; t_args[i] != NULL; i++) {
		/* -session for backwards compatibility */
		if (strcmp(t_args[i], "-session") == 0 ||
		    strcmp(t_args[i], "session=yes") == 0)
			module->pam_session = TRUE;
		else if (strcmp(t_args[i], "setcred=yes") == 0)
			module->pam_setcred = TRUE;
		else if (strncmp(t_args[i], "cache_key=", 10) == 0) {
			module->module.cache_key =
				auth_cache_parse_key(auth_passdb->auth->pool,
						     t_args[i] + 10);
		} else if (strcmp(t_args[i], "blocking=yes") == 0) {
			/* ignore, for backwards compatibility */
		} else if (strcmp(t_args[i], "failure_show_msg=yes") == 0) {
			module->failure_show_msg = TRUE;
		} else if (strcmp(t_args[i], "*") == 0) {
			/* for backwards compatibility */
			module->service_name = "%Ls";
		} else if (t_args[i+1] == NULL) {
			module->service_name =
				p_strdup(auth_passdb->auth->pool, t_args[i]);
		} else {
			i_fatal("Unexpected PAM parameter: %s", t_args[i]);
		}
	}
	return &module->module;
}

struct passdb_module_interface passdb_pam = {
	"pam",

	pam_preinit,
	NULL,
	NULL,

	pam_verify_plain,
	NULL,
	NULL
};

#endif
