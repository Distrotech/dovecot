/* Copyright (c) 2002-2013 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "hostpid.h"

#include <unistd.h>
#include <netdb.h>

#define HOSTNAME_DISALLOWED_CHARS "/\r\n\t"

const char *my_hostname = NULL;
const char *my_pid = NULL;

static char *my_domain = NULL;

void hostpid_init(void)
{
	static char hostname[256], pid[MAX_INT_STRLEN];

	if (gethostname(hostname, sizeof(hostname)-1) == -1)
		i_fatal("gethostname() failed: %m");
	hostname[sizeof(hostname)-1] = '\0';
	my_hostname = hostname;

	if (strcspn(hostname, HOSTNAME_DISALLOWED_CHARS) != strlen(hostname))
		i_fatal("Invalid system hostname: %s", hostname);

	/* allow calling hostpid_init() multiple times to reset hostname */
	i_free_and_null(my_domain);

	i_snprintf(pid, sizeof(pid), "%lld", (long long)getpid());
	my_pid = pid;
}

const char *my_hostdomain(void)
{
	struct hostent *hent;
	const char *name;

	if (my_domain == NULL) {
		hent = gethostbyname(my_hostname);
		name = hent != NULL ? hent->h_name : NULL;
		if (name == NULL) {
			/* failed, use just the hostname */
			name = my_hostname;
		}
		my_domain = i_strdup(name);
	}
	return my_domain;
}
