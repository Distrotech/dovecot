/* Copyright (c) 2002-2007 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "restrict-process-size.h"

#include <unistd.h>
#include <sys/time.h>
#ifdef HAVE_SYS_RESOURCE_H
#  include <sys/resource.h>
#endif

void restrict_process_size(unsigned int size ATTR_UNUSED,
			   unsigned int max_processes ATTR_UNUSED)
{
#ifdef HAVE_SETRLIMIT
	struct rlimit rlim;

#ifdef HAVE_RLIMIT_NPROC
	if (max_processes < INT_MAX) {
		rlim.rlim_max = rlim.rlim_cur = max_processes;
		if (setrlimit(RLIMIT_NPROC, &rlim) < 0)
			i_fatal("setrlimit(RLIMIT_NPROC, %u): %m", size);
	}
#endif

	if (size > 0 && size < INT_MAX/1024/1024) {
		rlim.rlim_max = rlim.rlim_cur = size*1024*1024;

		if (setrlimit(RLIMIT_DATA, &rlim) < 0)
			i_fatal("setrlimit(RLIMIT_DATA, %u): %m", size);

#ifdef HAVE_RLIMIT_AS
		if (setrlimit(RLIMIT_AS, &rlim) < 0)
			i_fatal("setrlimit(RLIMIT_AS, %u): %m", size);
#endif
	}
#else
	if (size != 0) {
		i_warning("Can't restrict process size: "
			  "setrlimit() not supported by system. "
			  "Set the limit to 0 to hide this warning.");
	}
#endif
}

void restrict_fd_limit(unsigned int count)
{
#ifdef HAVE_SETRLIMIT
	struct rlimit rlim;

	rlim.rlim_cur = rlim.rlim_max = count;
	if (setrlimit(RLIMIT_NOFILE, &rlim) < 0)
		i_fatal("setrlimit(RLIMIT_NOFILE, %u): %m", count);
#endif
}
