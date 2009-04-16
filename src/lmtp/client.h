#ifndef CLIENT_H
#define CLIENT_H

#include "network.h"

#define CLIENT_MAIL_DATA_MAX_INMEMORY_SIZE (1024*128)

struct mail_recipient {
	const char *name;
	struct mail_storage_service_multi_user *multi_user;
};

struct client_state {
	const char *mail_from;
	ARRAY_DEFINE(rcpt_to, struct mail_recipient);
	unsigned int rcpt_idx;

	unsigned int data_end_idx;

	/* Initially we start writing to mail_data. If it grows too large,
	   start using mail_data_fd. */
	buffer_t *mail_data;
	int mail_data_fd;
	struct ostream *mail_data_output;

	struct mailbox *raw_box;
	struct mailbox_transaction_context *raw_trans;
	struct mail *raw_mail;

	struct mail_user *dest_user;
	struct mail *first_saved_mail;
};

struct client {
	struct client *prev, *next;

	int fd_in, fd_out;
	struct io *io;
	struct istream *input;
	struct ostream *output;

	struct timeout *to_idle;
	time_t last_input;

	struct ip_addr remote_ip, local_ip;
	unsigned int remote_port, local_port;

	struct mail_user *raw_mail_user;
	const char *my_domain;

	pool_t state_pool;
	struct client_state state;

	unsigned int disconnected:1;
};

extern unsigned int clients_count;

struct client *client_create(int fd_in, int fd_out);
void client_destroy(struct client *client, const char *prefix,
		    const char *reason);
void client_disconnect(struct client *client, const char *prefix,
		       const char *reason);
void client_state_reset(struct client *client);

void client_input(struct client *client);
void client_input_handle(struct client *client);
int client_input_read(struct client *client);

void client_send_line(struct client *client, const char *fmt, ...)
	ATTR_FORMAT(2, 3);

void clients_destroy(void);

#endif
