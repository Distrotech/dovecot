#ifndef __SSL_PROXY_H
#define __SSL_PROXY_H

struct ip_addr;
struct ssl_proxy;

extern int ssl_initialized;

/* establish SSL connection with the given fd, returns a new fd which you
   must use from now on, or -1 if error occured. Unless -1 is returned,
   the given fd must be simply forgotten. */
int ssl_proxy_new(int fd, struct ip_addr *ip, struct ssl_proxy **proxy_r);
int ssl_proxy_has_valid_client_cert(struct ssl_proxy *proxy);
void ssl_proxy_free(struct ssl_proxy *proxy);

void ssl_proxy_init(void);
void ssl_proxy_deinit(void);

#endif
