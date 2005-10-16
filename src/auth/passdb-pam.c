/*
   Based on auth_pam.c from popa3d by Solar Designer <solar@openwall.com>.

   You're allowed to do whatever you like with this software (including
   re-distribution in source and/or binary form, with or without
   modification), provided that credit is given where it is due and any
   modified versions are marked as such.  There's absolutely no warranty.
*/

#include "common.h"

#ifdef PASSDB_PAM

#include "buffer.h"
#include "ioloop.h"
#include "network.h"
#include "passdb.h"
#include "mycrypt.h"
#include "safe-memset.h"

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

#ifdef HAVE_SECURITY_PAM_APPL_H
#  include <security/pam_appl.h>
#elif defined(HAVE_PAM_PAM_APPL_H)
#  include <pam/pam_appl.h>
#endif

#if !defined(_SECURITY_PAM_APPL_H) && !defined(LINUX_PAM) && !defined(_OPENPAM)
/* Sun's PAM doesn't use const. we use a bit dirty hack to check it.
   Originally it was just __sun__ check, but HP/UX also uses Sun's PAM
   so I thought this might work better. */
#  define linux_const
#else
#  define linux_const			const
#endif
typedef linux_const void *pam_item_t;

#ifdef AUTH_PAM_USERPASS
#  include <security/pam_client.h>

#  ifndef PAM_BP_RCONTROL
/* Linux-PAM prior to 0.74 */
#    define PAM_BP_RCONTROL	PAM_BP_CONTROL
#    define PAM_BP_WDATA	PAM_BP_DATA
#    define PAM_BP_RDATA	PAM_BP_DATA
#  endif

#  define USERPASS_AGENT_ID		"userpass"
#  define USERPASS_AGENT_ID_LENGTH	8

#  define USERPASS_USER_MASK		0x03
#  define USERPASS_USER_REQUIRED	1
#  define USERPASS_USER_KNOWN		2
#  define USERPASS_USER_FIXED		3
#endif

struct pam_auth_request {
	int fd;
	struct io *io;

	struct auth_request *request;
        verify_plain_callback_t *callback;
};

struct pam_userpass {
	const char *user;
	const char *pass;
};

extern struct passdb_module passdb_pam;

static int pam_session;
static char *service_name, *pam_cache_key;
static struct timeout *to_wait;

static int pam_userpass_conv(int num_msg, linux_const struct pam_message **msg,
	struct pam_response **resp, void *appdata_ptr)
{
	/* @UNSAFE */
	struct pam_userpass *userpass = (struct pam_userpass *) appdata_ptr;
#ifdef AUTH_PAM_USERPASS
	pamc_bp_t prompt;
	const char *input;
	char *output;
	char flags;
	size_t userlen, passlen;

	if (num_msg != 1 || msg[0]->msg_style != PAM_BINARY_PROMPT)
		return PAM_CONV_ERR;

	prompt = (pamc_bp_t)msg[0]->msg;
	input = PAM_BP_RDATA(prompt);

	if (PAM_BP_RCONTROL(prompt) != PAM_BPC_SELECT ||
	    strncmp(input, USERPASS_AGENT_ID "/", USERPASS_AGENT_ID_LENGTH + 1))
		return PAM_CONV_ERR;

	flags = input[USERPASS_AGENT_ID_LENGTH + 1];
	input += USERPASS_AGENT_ID_LENGTH + 1 + 1;

	if ((flags & USERPASS_USER_MASK) == USERPASS_USER_FIXED &&
	    strcmp(input, userpass->user))
		return PAM_CONV_AGAIN;

	if (!(*resp = malloc(sizeof(struct pam_response))))
		return PAM_CONV_ERR;

	userlen = strlen(userpass->user);
	passlen = strlen(userpass->pass);

	prompt = NULL;
	PAM_BP_RENEW(&prompt, PAM_BPC_DONE, userlen + 1 + passlen);
	output = PAM_BP_WDATA(prompt);

	memcpy(output, userpass->user, userlen + 1);
	memcpy(output + userlen + 1, userpass->pass, passlen);

	(*resp)[0].resp_retcode = 0;
	(*resp)[0].resp = (char *)prompt;
#else
	char *string;
	int i;

	if (!(*resp = malloc(num_msg * sizeof(struct pam_response))))
		return PAM_CONV_ERR;

	for (i = 0; i < num_msg; i++) {
		switch (msg[i]->msg_style) {
		case PAM_PROMPT_ECHO_ON:
			string = strdup(userpass->user);
			if (string == NULL)
				i_fatal_status(FATAL_OUTOFMEM, "Out of memory");
			break;
		case PAM_PROMPT_ECHO_OFF:
			string = strdup(userpass->pass);
			if (string == NULL)
				i_fatal_status(FATAL_OUTOFMEM, "Out of memory");
			break;
		case PAM_ERROR_MSG:
		case PAM_TEXT_INFO:
			string = NULL;
			break;
		default:
			while (--i >= 0) {
				if ((*resp)[i].resp == NULL)
					continue;
				safe_memset((*resp)[i].resp, 0,
					    strlen((*resp)[i].resp));
				free((*resp)[i].resp);
				(*resp)[i].resp = NULL;
			}

			free(*resp);
			*resp = NULL;

			return PAM_CONV_ERR;
		}

		(*resp)[i].resp_retcode = PAM_SUCCESS;
		(*resp)[i].resp = string;
	}
#endif

	return PAM_SUCCESS;
}

static int pam_auth(struct auth_request *request,
		    pam_handle_t *pamh, const char **error)
{
	void *item;
	int status;

	*error = NULL;

	if ((status = pam_authenticate(pamh, 0)) != PAM_SUCCESS) {
		*error = t_strdup_printf("pam_authenticate() failed: %s",
					 pam_strerror(pamh, status));
		return status;
	}

#ifdef HAVE_PAM_SETCRED
	if ((status = pam_setcred(pamh, PAM_ESTABLISH_CRED)) != PAM_SUCCESS) {
		*error = t_strdup_printf("pam_setcred() failed: %s",
					 pam_strerror(pamh, status));
		return status;
	}
#endif

	if ((status = pam_acct_mgmt(pamh, 0)) != PAM_SUCCESS) {
		*error = t_strdup_printf("pam_acct_mgmt() failed: %s",
					 pam_strerror(pamh, status));
		return status;
	}

	if (pam_session) {
	        if ((status = pam_open_session(pamh, 0)) != PAM_SUCCESS) {
			*error = t_strdup_printf(
					"pam_open_session() failed: %s",
					pam_strerror(pamh, status));
	                return status;
	        }

	        if ((status = pam_close_session(pamh, 0)) != PAM_SUCCESS) {
			*error = t_strdup_printf(
					"pam_close_session() failed: %s",
	                                pam_strerror(pamh, status));
	                return status;
	        }
	}

	status = pam_get_item(pamh, PAM_USER, (linux_const void **)&item);
	if (status != PAM_SUCCESS) {
		*error = t_strdup_printf("pam_get_item() failed: %s",
					 pam_strerror(pamh, status));
		return status;
	}
        auth_request_set_field(request, "user", item, NULL);

	return PAM_SUCCESS;
}

static void
pam_verify_plain_child(struct auth_request *request, const char *service,
		       const char *password, int fd)
{
	pam_handle_t *pamh;
	struct pam_userpass userpass;
	struct pam_conv conv;
	enum passdb_result result;
	int status, status2;
	const char *str;
	char buf_data[512];
	buffer_t *buf;

	conv.conv = pam_userpass_conv;
	conv.appdata_ptr = &userpass;

	userpass.user = request->user;
	userpass.pass = password;

	status = pam_start(service, request->user, &conv, &pamh);
	if (status != PAM_SUCCESS) {
		result = PASSDB_RESULT_INTERNAL_FAILURE;
		str = t_strdup_printf("pam_start() failed: %s",
				      pam_strerror(pamh, status));
	} else {
#ifdef PAM_RHOST
		const char *host = net_ip2addr(&request->remote_ip);
		if (host != NULL)
			pam_set_item(pamh, PAM_RHOST, host);
#endif

		status = pam_auth(request, pamh, &str);
		if ((status2 = pam_end(pamh, status)) == PAM_SUCCESS) {
			/* FIXME: check for PASSDB_RESULT_UNKNOWN_USER
			   somehow? */
			result = status == PAM_SUCCESS ? PASSDB_RESULT_OK :
				PASSDB_RESULT_PASSWORD_MISMATCH;
		} else {
			result = PASSDB_RESULT_INTERNAL_FAILURE;
			str = t_strdup_printf("pam_end() failed: %s",
					      pam_strerror(pamh, status2));
		}
	}

	buf = buffer_create_data(pool_datastack_create(),
				 buf_data, sizeof(buf_data));
	buffer_append(buf, &result, sizeof(result));

	if (str != NULL) {
		/* may truncate the error. tough luck. */
		buffer_append(buf, str, strlen(str));
	}

	write(fd, buf_data, buffer_get_used_size(buf));
}

static void pam_child_input(void *context)
{
	struct pam_auth_request *request = context;
	struct auth_request *auth_request = request->request;
	enum passdb_result result;
	char buf[513];
	ssize_t ret;

	/* POSIX guarantees that writing 512 bytes or less to pipes is atomic.
	   We rely on that. */
	ret = read(request->fd, buf, sizeof(buf)-1);
	if (ret < 0) {
		auth_request_log_error(auth_request, "pam",
				       "read() from child process failed: %m");
		result = PASSDB_RESULT_INTERNAL_FAILURE;
	} else if (ret == 0) {
		/* it died */
		auth_request_log_error(auth_request, "pam",
				       "Child process died");
		result = PASSDB_RESULT_INTERNAL_FAILURE;
	} else if ((size_t)ret < sizeof(result)) {
		auth_request_log_error(auth_request, "pam",
			"Child process returned only %d bytes", (int)ret);
		result = PASSDB_RESULT_INTERNAL_FAILURE;
	} else {
		memcpy(&result, buf, sizeof(result));

		if ((size_t)ret > sizeof(result)) {
			/* error message included */
			buf[ret] = '\0';

			if (result == PASSDB_RESULT_INTERNAL_FAILURE) {
				auth_request_log_error(auth_request, "pam",
					"%s", buf + sizeof(result));
			} else {
				auth_request_log_info(auth_request, "pam",
					"%s", buf + sizeof(result));
			}
		}
	}

	if (close(request->fd) < 0) {
		auth_request_log_error(auth_request, "pam",
				       "close(child input) failed: %m");
	}

	if (auth_request_unref(auth_request))
		request->callback(result, auth_request);

	io_remove(request->io);
	i_free(request);
}

static void wait_timeout(void *context __attr_unused__)
{
	int status;
	pid_t pid;

	/* FIXME: if we ever do some other kind of forking, this needs fixing */
	while ((pid = waitpid(-1, &status, WNOHANG)) != 0) {
		if (pid == -1) {
			if (errno == ECHILD) {
				timeout_remove(to_wait);
				to_wait = NULL;
			} else if (errno != EINTR)
				i_error("waitpid() failed: %m");
			return;
		}

		if (WIFSIGNALED(status)) {
			i_error("PAM: Child %s died with signal %d",
				dec2str(pid), WTERMSIG(status));
		}
	}
}

static void
pam_verify_plain(struct auth_request *request, const char *password,
		 verify_plain_callback_t *callback)
{
        struct pam_auth_request *pam_auth_request;
	const char *service;
	int fd[2];
	pid_t pid;

	service = service_name != NULL ? service_name : request->service;
	if (pipe(fd) < 0) {
		auth_request_log_error(request, "pam", "pipe() failed: %m");
		callback(PASSDB_RESULT_INTERNAL_FAILURE, request);
		return;
	}

	pid = fork();
	if (pid == -1) {
		auth_request_log_error(request, "pam", "fork() failed: %m");
		callback(PASSDB_RESULT_INTERNAL_FAILURE, request);
		(void)close(fd[0]);
		(void)close(fd[1]);
		return;
	}

	if (pid == 0) {
		(void)close(fd[0]);
		pam_verify_plain_child(request, service, password, fd[1]);
		_exit(0);
	}

	if (close(fd[1]) < 0) {
		auth_request_log_error(request, "pam",
				       "close(fd[1]) failed: %m");
	}

	auth_request_ref(request);
	pam_auth_request = i_new(struct pam_auth_request, 1);
	pam_auth_request->fd = fd[0];
	pam_auth_request->request = request;
	pam_auth_request->callback = callback;

	pam_auth_request->io =
		io_add(fd[0], IO_READ, pam_child_input, pam_auth_request);

	if (to_wait == NULL)
		to_wait = timeout_add(1000, wait_timeout, NULL);
}

static void pam_init(const char *args)
{
	const char *const *t_args;
	int i;

	pam_session = FALSE;
	service_name = i_strdup("dovecot");
        pam_cache_key = NULL;

	t_push();
	t_args = t_strsplit(args, " ");
        for(i = 0; t_args[i] != NULL; i++) {
		if (strcmp(t_args[i], "-session") == 0)
			pam_session = TRUE;
		else if (strncmp(t_args[i], "cache_key=", 10) == 0) {
			i_free(pam_cache_key);
			pam_cache_key = i_strdup(t_args[i] + 10);
		} else if (strcmp(t_args[i], "*") == 0) {
			i_free(service_name);
			service_name = NULL;
		} else {
			if (*t_args[i] != '\0') {
				i_free(service_name);
				service_name = i_strdup(t_args[i]);
			}
		}
	}
	t_pop();

	to_wait = NULL;
        passdb_pam.cache_key = pam_cache_key;
}

static void pam_deinit(void)
{
	if (to_wait != NULL)
		timeout_remove(to_wait);
	i_free(service_name);
	i_free(pam_cache_key);
}

struct passdb_module passdb_pam = {
	"pam",
	NULL, NULL, FALSE,

	NULL,
	pam_init,
	pam_deinit,

	pam_verify_plain,
	NULL
};

#endif
