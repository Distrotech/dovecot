#ifndef __SYSLOG_UTIL_H
#define __SYSLOG_UTIL_H

struct syslog_facility_list {
	const char *name;
	int facility;
};

extern struct syslog_facility_list syslog_facilities[];

/* Returns TRUE if found. */
int syslog_facility_find(const char *name, int *facility_r);

#endif
