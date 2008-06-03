#ifndef MAIL_SEARCH_BUILD_H
#define MAIL_SEARCH_BUILD_H

#include "mail-search.h"

struct imap_arg;
struct mailbox;

struct mail_search_args *mail_search_build_init(void);

int mail_search_build_from_imap_args(const struct imap_arg *imap_args,
				     const char *charset,
				     struct mail_search_args **args_r,
				     const char **error_r);

void mail_search_build_add_all(struct mail_search_args *args);
void mail_search_build_add_seqset(struct mail_search_args *args,
				  uint32_t seq1, uint32_t seq2);

#endif
