#ifndef __FAILURES_H
#define __FAILURES_H

/* Default exit status codes that we could use. */
enum fatal_exit_status {
	FATAL_LOGOPEN	= 80, /* Can't open log file */
	FATAL_LOGWRITE  = 81, /* Can't write to log file */
	FATAL_LOGERROR  = 82, /* Internal logging error */
	FATAL_OUTOFMEM	= 83, /* Out of memory */
	FATAL_EXEC	= 84, /* exec() failed */

	FATAL_DEFAULT	= 89
};

#define DEFAULT_FAILURE_STAMP_FORMAT "%b %d %H:%M:%S "

typedef void (*failure_callback_t)(const char *, va_list);
typedef void (*fatal_failure_callback_t)(int status, const char *, va_list);

void i_panic(const char *format, ...) __attr_format__(1, 2) __attr_noreturn__;
void i_fatal(const char *format, ...) __attr_format__(1, 2) __attr_noreturn__;
void i_error(const char *format, ...) __attr_format__(1, 2);
void i_warning(const char *format, ...) __attr_format__(1, 2);
void i_info(const char *format, ...) __attr_format__(1, 2);

void i_fatal_status(int status, const char *format, ...)
	__attr_format__(2, 3) __attr_noreturn__;

/* Change failure handlers. Make sure they don't modify errno. */
void i_set_panic_handler(failure_callback_t callback __attr_noreturn__);
void i_set_fatal_handler(fatal_failure_callback_t callback __attr_noreturn__);
void i_set_error_handler(failure_callback_t callback);
void i_set_warning_handler(failure_callback_t callback);
void i_set_info_handler(failure_callback_t callback);

/* Send failures to syslog() */
void i_syslog_panic_handler(const char *fmt, va_list args) __attr_noreturn__;
void i_syslog_fatal_handler(int status, const char *fmt, va_list args)
	__attr_noreturn__;
void i_syslog_error_handler(const char *fmt, va_list args);
void i_syslog_warning_handler(const char *fmt, va_list args);
void i_syslog_info_handler(const char *fmt, va_list args);

/* Open syslog and set failure/info handlers to use it. */
void i_set_failure_syslog(const char *ident, int options, int facility);

/* Send failures to specified log file instead of stderr. */
void i_set_failure_file(const char *path, const char *prefix);

/* Send informational messages to specified log file. i_set_failure_*()
   functions modify the info file too, so call this function after them. */
void i_set_info_file(const char *path);

/* Prefix failures with a timestamp. fmt is in strftime() format. */
void i_set_failure_timestamp_format(const char *fmt);

void failures_deinit(void);

#endif
