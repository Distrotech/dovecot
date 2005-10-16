#ifndef __PASSDB_CACHE_H
#define __PASSDB_CACHE_H

#include "auth-cache.h"

enum passdb_result;
extern struct auth_cache *passdb_cache;

int passdb_cache_verify_plain(struct auth_request *request, const char *key,
			      const char *password,
			      enum passdb_result *result_r, int use_expired);
int passdb_cache_lookup_credentials(struct auth_request *request,
				    const char *key, const char **password_r,
				    const char **scheme_r,
				    enum passdb_result *result_r,
				    int use_expired);

void passdb_cache_init(void);
void passdb_cache_deinit(void);

#endif
