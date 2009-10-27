#ifndef MASTER_LOGIN_AUTH_H
#define MASTER_LOGIN_AUTH_H

struct master_auth_request;

typedef void
master_login_auth_request_callback_t(const char *const *auth_args,
				     void *context);

struct master_login_auth *master_login_auth_init(const char *auth_socket_path);
void master_login_auth_deinit(struct master_login_auth **auth);

void master_login_auth_request(struct master_login_auth *auth,
			       const struct master_auth_request *req,
			       master_login_auth_request_callback_t *callback,
			       void *context);
unsigned int master_login_auth_request_count(struct master_login_auth *auth);

#endif
