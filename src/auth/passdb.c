/* Copyright (C) 2002-2003 Timo Sirainen */

#include "common.h"
#include "mech.h"
#include "auth-module.h"
#include "passdb.h"

#include <stdlib.h>

#ifdef AUTH_MODULES
static struct auth_module *passdb_module = NULL;
#endif

struct passdb_module *passdb;

static const char *
passdb_credentials_to_str(enum passdb_credentials credentials)
{
	switch (credentials) {
	case _PASSDB_CREDENTIALS_INTERNAL:
		break;
	case PASSDB_CREDENTIALS_PLAINTEXT:
		return "PLAIN";
	case PASSDB_CREDENTIALS_CRYPT:
		return "CRYPT";
	case PASSDB_CREDENTIALS_DIGEST_MD5:
		return "DIGEST-MD5";
	}

	return "??";
}

void passdb_handle_credentials(enum passdb_credentials credentials,
			       const char *user, const char *password,
			       const char *scheme,
			       lookup_credentials_callback_t *callback,
                               struct auth_request *auth_request)
{
	const char *wanted_scheme;

	if (credentials == PASSDB_CREDENTIALS_CRYPT) {
		/* anything goes */
		if (password != NULL)
			password = t_strdup_printf("{%s}%s", scheme, password);
		callback(password, auth_request);
		return;
	}

	if (password != NULL) {
		wanted_scheme = passdb_credentials_to_str(credentials);
		if (strcasecmp(scheme, wanted_scheme) != 0) {
			if (verbose) {
				i_info("password(%s): Requested %s scheme, "
				       "but we have only %s", user,
				       wanted_scheme, scheme);
			}
			password = NULL;
		}
	}

	callback(password, auth_request);
}

void passdb_init(void)
{
	const char *name, *args;

	passdb = NULL;

	name = getenv("PASSDB");
	if (name == NULL)
		i_fatal("PASSDB environment is unset");

	args = strchr(name, ' ');
	name = t_strcut(name, ' ');

#ifdef PASSDB_PASSWD
	if (strcasecmp(name, "passwd") == 0)
		passdb = &passdb_passwd;
#endif
#ifdef PASSDB_PASSWD_FILE
	if (strcasecmp(name, "passwd-file") == 0)
		passdb = &passdb_passwd_file;
#endif
#ifdef PASSDB_PAM
	if (strcasecmp(name, "pam") == 0)
		passdb = &passdb_pam;
#endif
#ifdef PASSDB_SHADOW
	if (strcasecmp(name, "shadow") == 0)
		passdb = &passdb_shadow;
#endif
#ifdef PASSDB_VPOPMAIL
	if (strcasecmp(name, "vpopmail") == 0)
		passdb = &passdb_vpopmail;
#endif
#ifdef PASSDB_LDAP
	if (strcasecmp(name, "ldap") == 0)
		passdb = &passdb_ldap;
#endif
#ifdef AUTH_MODULES
	passdb_module = auth_module_open(name);
	if (passdb_module != NULL) {
		passdb = auth_module_sym(passdb_module,
					 t_strconcat("passdb_", name, NULL));
	}
#endif

	if (passdb == NULL)
		i_fatal("Unknown passdb type '%s'", name);

	/* initialize */
	if (passdb->init != NULL)
		passdb->init(args != NULL ? args+1 : "");

	if ((auth_mechanisms & AUTH_MECH_PLAIN) &&
	    passdb->verify_plain == NULL)
		i_fatal("Passdb %s doesn't support PLAIN method", name);

	if ((auth_mechanisms & AUTH_MECH_DIGEST_MD5) &&
	    passdb->lookup_credentials == NULL)
		i_fatal("Passdb %s doesn't support DIGEST-MD5 method", name);
}

void passdb_deinit(void)
{
	if (passdb != NULL && passdb->deinit != NULL)
		passdb->deinit();
#ifdef AUTH_MODULES
	if (passdb_module != NULL)
                auth_module_close(passdb_module);
#endif
}
