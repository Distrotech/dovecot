/* Copyright (C) 2003 Alex Howansky, Timo Sirainen */

#include "config.h"
#undef HAVE_CONFIG_H

#if defined(PASSDB_MYSQL) || defined(USERDB_MYSQL)

#include "common.h"
#include "network.h"
#include "str.h"
#include "settings.h"
#include "db-mysql.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>

#define DEF(type, name) { type, #name, offsetof(struct mysql_settings, name) }

static struct setting_def setting_defs[] = {
	DEF(SET_STR, db_host),
	DEF(SET_STR, db_port),
	DEF(SET_STR, db_unix_socket),
	DEF(SET_STR, db),
	DEF(SET_STR, db_user),
	DEF(SET_STR, db_passwd),
	DEF(SET_STR, db_client_flags),
	DEF(SET_STR, password_query),
	DEF(SET_STR, user_query),
	DEF(SET_STR, default_pass_scheme)
};

struct mysql_settings default_mysql_settings = {
	MEMBER(db_host) "localhost",
	MEMBER(db_port) "0",
	MEMBER(db_unix_socket) "/var/tmp/mysql.sock",
	MEMBER(db) "email_accounts",
	MEMBER(db_user) "dovecot",
	MEMBER(db_passwd) "changeme",
	MEMBER(db_client_flags) "0",
	MEMBER(password_query) "SELECT password FROM users WHERE userid = '%u'",
	MEMBER(user_query) "SELECT home, uid, gid FROM users WHERE userid = '%u'",
	MEMBER(default_pass_scheme) "PLAIN-MD5"
};

static struct mysql_connection *mysql_connections = NULL;

static int mysql_conn_open(struct mysql_connection *conn);
static void mysql_conn_close(struct mysql_connection *conn);

void db_mysql_query(struct mysql_connection *conn, const char *query,
		    struct mysql_request *request)
{
	MYSQL_RES *res;
	int failed;

	if (!conn->connected) {
		if (!mysql_conn_open(conn)) {
			request->callback(conn, request, NULL);
			return;
		}
	}

	if (verbose_debug)
		i_info("MYSQL: Performing query: %s", query);

	if (mysql_query(conn->mysql, query))
		i_info("MYSQL: Error executing query \"%s\": %s", query,
		       mysql_error(conn->mysql));

	if ((res = mysql_store_result(conn->mysql)))
		failed = FALSE;
	else {
		i_info("MYSQL: Error retrieving results: %s",
		       mysql_error(conn->mysql));
		failed = TRUE;
	}

	request->callback(conn, request, failed ? NULL : res);
	mysql_free_result(res);
	i_free(request);
}

static int mysql_conn_open(struct mysql_connection *conn)
{
	if (conn->connected)
		return TRUE;

	if (conn->mysql == NULL) {
		conn->mysql = mysql_init(NULL);
		if (conn->mysql == NULL) {
			i_error("MYSQL: mysql_init failed");
			return FALSE;
		}

		if (!mysql_real_connect(conn->mysql, conn->set.db_host,
					conn->set.db_user, conn->set.db_passwd,
					conn->set.db,
					atoi(conn->set.db_port),
					conn->set.db_unix_socket,
					strtoul(conn->set.db_client_flags,
						NULL, 10))) {
			i_error("MYSQL: Can't connect to database %s: %s",
				conn->set.db, mysql_error(conn->mysql));
			return FALSE;
		}
	}

	conn->connected = TRUE;
	return TRUE;
}

static void mysql_conn_close(struct mysql_connection *conn)
{
	conn->connected = FALSE;

	if (conn->mysql != NULL) {
		mysql_close(conn->mysql);
		conn->mysql = NULL;
	}
}

static struct mysql_connection *mysql_conn_find(const char *config_path)
{
	struct mysql_connection *conn;

	for (conn = mysql_connections; conn != NULL; conn = conn->next) {
		if (strcmp(conn->config_path, config_path) == 0)
			return conn;
	}

	return NULL;
}

static const char *parse_setting(const char *key, const char *value,
				 void *context)
{
	struct mysql_connection *conn = context;

	return parse_setting_from_defs(conn->pool, setting_defs,
				       &conn->set, key, value);
}

struct mysql_connection *db_mysql_init(const char *config_path)
{
	struct mysql_connection *conn;
	pool_t pool;

	conn = mysql_conn_find(config_path);
	if (conn != NULL) {
		conn->refcount++;
		return conn;
	}

	pool = pool_alloconly_create("mysql_connection", 1024);
	conn = p_new(pool, struct mysql_connection, 1);
	conn->pool = pool;

	conn->refcount = 1;

	conn->config_path = p_strdup(pool, config_path);
	conn->set = default_mysql_settings;
	if (!settings_read(config_path, NULL, parse_setting, NULL, conn))
		exit(FATAL_DEFAULT);

	(void)mysql_conn_open(conn);

	conn->next = mysql_connections;
	mysql_connections = conn;
	return conn;
}

void db_mysql_unref(struct mysql_connection *conn)
{
	if (--conn->refcount > 0)
		return;

	mysql_conn_close(conn);
	pool_unref(conn->pool);
}

#endif
