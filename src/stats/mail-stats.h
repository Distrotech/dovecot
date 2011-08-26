#ifndef MAIL_STATS_H
#define MAIL_STATS_H

#include "network.h"
#include "guid.h"

struct mail_stats {
	struct timeval cpu_secs;
	uint64_t disk_input, disk_output;

	uint32_t lookup_path, lookup_attr, read_count, cache_hits;
	uint64_t read_bytes;
};

struct mail_command {
	struct mail_command *stable_prev, *stable_next;
	struct mail_command *session_prev, *session_next;

	struct mail_session *session;
	char *name, *args;
	/* non-zero id means the command is still running */
	unsigned int id;

	time_t last_update;
	struct mail_stats stats;

	int refcount;
};

struct mail_session {
	struct mail_session *stable_prev, *stable_next;
	struct mail_session *sorted_prev, *sorted_next;
	struct mail_session *user_prev, *user_next;
	struct mail_session *ip_prev, *ip_next;

	/* if guid is empty, the session no longer exists */
	guid_128_t guid;
	struct mail_user *user;
	char *service;
	/* ip address may be NULL if there's none */
	struct mail_ip *ip;
	struct timeout *to_idle;

	struct mail_stats stats;
	time_t last_update;
	unsigned int num_cmds;

	bool disconnected;
	unsigned int highest_cmd_id;
	int refcount;
	struct mail_command *commands;
};

struct mail_user {
	struct mail_user *stable_prev, *stable_next;
	struct mail_user *sorted_prev, *sorted_next;
	struct mail_user *domain_prev, *domain_next;
	char *name;
	struct mail_domain *domain;
	time_t reset_timestamp;

	time_t last_update;
	struct mail_stats stats;
	unsigned int num_logins;
	unsigned int num_cmds;

	int refcount;
	struct mail_session *sessions;
};

struct mail_domain {
	struct mail_domain *stable_prev, *stable_next;
	struct mail_domain *sorted_prev, *sorted_next;
	char *name;
	time_t reset_timestamp;

	time_t last_update;
	struct mail_stats stats;
	unsigned int num_logins;
	unsigned int num_cmds;

	int refcount;
	struct mail_user *users;
};

struct mail_ip {
	struct mail_ip *stable_prev, *stable_next;
	struct mail_ip *sorted_prev, *sorted_next;
	struct ip_addr ip;
	time_t reset_timestamp;

	time_t last_update;
	struct mail_stats stats;
	unsigned int num_logins;
	unsigned int num_cmds;

	int refcount;
	struct mail_session *sessions;
};

int mail_stats_parse(const char *const *args, struct mail_stats *stats_r,
		     const char **error_r);
/* diff1 is supposed to have smaller values than diff2. Returns TRUE if this
   is so, FALSE if not */
bool mail_stats_diff(const struct mail_stats *stats1,
		     const struct mail_stats *stats2,
		     struct mail_stats *diff_stats_r);
void mail_stats_add(struct mail_stats *dest, const struct mail_stats *src);

#endif
