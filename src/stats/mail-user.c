/* Copyright (c) 2011 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "ioloop.h"
#include "hash.h"
#include "llist.h"
#include "global-memory.h"
#include "stats-settings.h"
#include "mail-stats.h"
#include "mail-domain.h"
#include "mail-user.h"

static struct hash_table *mail_users_hash;
/* users are sorted by their last_update timestamp, oldest first */
static struct mail_user *mail_users_head, *mail_users_tail;
struct mail_user *stable_mail_users;

static size_t mail_user_memsize(const struct mail_user *user)
{
	return sizeof(*user) + strlen(user->name) + 1;
}

struct mail_user *mail_user_login(const char *username)
{
	struct mail_user *user;
	const char *domain;

	user = hash_table_lookup(mail_users_hash, username);
	if (user != NULL) {
		user->num_logins++;
		user->domain->num_logins++;
		mail_user_refresh(user, NULL);
		return user;
	}

	domain = strchr(username, '@');
	if (domain != NULL)
		domain++;
	else
		domain = "";

	user = i_new(struct mail_user, 1);
	user->name = i_strdup(username);
	user->reset_timestamp = ioloop_time;
	user->domain = mail_domain_login(domain);

	hash_table_insert(mail_users_hash, user->name, user);
	DLLIST_PREPEND_FULL(&stable_mail_users, user,
			    stable_prev, stable_next);
	DLLIST2_APPEND_FULL(&mail_users_head, &mail_users_tail, user,
			    sorted_prev, sorted_next);
	DLLIST_PREPEND_FULL(&user->domain->users, user,
			    domain_prev, domain_next);
	mail_domain_ref(user->domain);

	user->num_logins++;
	user->last_update = ioloop_timeval;
	global_memory_alloc(mail_user_memsize(user));
	return user;
}

struct mail_user *mail_user_lookup(const char *username)
{
	return hash_table_lookup(mail_users_hash, username);
}

void mail_user_ref(struct mail_user *user)
{
	user->refcount++;
}

void mail_user_unref(struct mail_user **_user)
{
	struct mail_user *user = *_user;

	i_assert(user->refcount > 0);
	user->refcount--;

	*_user = NULL;
}

static void mail_user_free(struct mail_user *user)
{
	i_assert(user->refcount == 0);
	i_assert(user->sessions == NULL);

	global_memory_free(mail_user_memsize(user));
	DLLIST_REMOVE_FULL(&stable_mail_users, user,
			   stable_prev, stable_next);
	DLLIST2_REMOVE_FULL(&mail_users_head, &mail_users_tail, user,
			    sorted_prev, sorted_next);
	DLLIST_REMOVE_FULL(&user->domain->users, user,
			   domain_prev, domain_next);
	mail_domain_unref(&user->domain);

	i_free(user->name);
	i_free(user);
}

void mail_user_refresh(struct mail_user *user,
		       const struct mail_stats *diff_stats)
{
	if (diff_stats != NULL)
		mail_stats_add(&user->stats, diff_stats);
	user->last_update = ioloop_timeval;
	DLLIST2_REMOVE_FULL(&mail_users_head, &mail_users_tail, user,
			    sorted_prev, sorted_next);
	DLLIST2_APPEND_FULL(&mail_users_head, &mail_users_tail, user,
			    sorted_prev, sorted_next);
	mail_domain_refresh(user->domain, diff_stats);
}

void mail_users_free_memory(void)
{
	while (mail_users_head != NULL && mail_users_head->refcount == 0) {
		mail_user_free(mail_users_head);

		if (global_used_memory < stats_settings->memory_limit)
			break;
		if (ioloop_time -
		    mail_users_head->last_update.tv_sec < stats_settings->user_min_time)
			break;
	}
}

void mail_users_init(void)
{
	mail_users_hash =
		hash_table_create(default_pool, default_pool, 0,
				  str_hash, (hash_cmp_callback_t *)strcmp);
}

void mail_users_deinit(void)
{
	while (mail_users_head != NULL)
		mail_user_free(mail_users_head);
	hash_table_destroy(&mail_users_hash);
}
