/* Copyright (C) 2003 Timo Sirainen */

#include "lib.h"
#include "str.h"
#include "hex-binary.h"
#include "hostpid.h"
#include "randgen.h"
#include "write-full.h"
#include "file-dotlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>

#define DEFAULT_LOCK_SUFFIX ".lock"

/* 0.1 .. 0.2msec */
#define LOCK_RANDOM_USLEEP_TIME (100000 + (unsigned int)rand() % 100000)

struct dotlock {
	struct dotlock_settings settings;

	dev_t dev;
	ino_t ino;
	time_t mtime;

	char *path;
	int fd;
};

struct lock_info {
	const struct dotlock_settings *set;
	const char *path, *lock_path, *temp_path;
	int fd;

	dev_t dev;
	ino_t ino;
	off_t size;
	time_t ctime, mtime;

	off_t last_size;
	time_t last_ctime, last_mtime;
	time_t last_change;

	int have_pid;
	time_t last_pid_check;
};

static struct dotlock *
file_dotlock_alloc(const struct dotlock_settings *settings)
{
	struct dotlock *dotlock;

	dotlock = i_new(struct dotlock, 1);
	dotlock->settings = *settings;
	if (dotlock->settings.lock_suffix == NULL)
		dotlock->settings.lock_suffix = DEFAULT_LOCK_SUFFIX;
	dotlock->fd = -1;

	return dotlock;
}

static pid_t read_local_pid(const char *lock_path)
{
	char buf[512], *host;
	int fd;
	ssize_t ret;

	fd = open(lock_path, O_RDONLY);
	if (fd == -1)
		return -1; /* ignore the actual error */

	/* read line */
	ret = read(fd, buf, sizeof(buf)-1);
	(void)close(fd);
	if (ret <= 0)
		return -1;

	/* fix the string */
	if (buf[ret-1] == '\n')
		ret--;
	buf[ret] = '\0';

	/* it should contain pid:host */
	host = strchr(buf, ':');
	if (host == NULL)
		return -1;
	*host++ = '\0';

	/* host must be ours */
	if (strcmp(host, my_hostname) != 0)
		return -1;

	if (!is_numeric(buf, '\0'))
		return -1;
	return (pid_t)strtoul(buf, NULL, 0);
}

static int check_lock(time_t now, struct lock_info *lock_info)
{
	time_t immediate_stale_timeout =
		lock_info->set->immediate_stale_timeout;
	time_t stale_timeout = lock_info->set->stale_timeout;
	struct stat st;
	pid_t pid;

	if (lstat(lock_info->lock_path, &st) < 0) {
		if (errno != ENOENT) {
			i_error("lstat(%s) failed: %m", lock_info->lock_path);
			return -1;
		}
		return 1;
	}

	if (lock_info->set->immediate_stale_timeout != 0 &&
	    now > st.st_mtime + immediate_stale_timeout &&
	    now > st.st_ctime + immediate_stale_timeout) {
		/* old lock file */
		if (unlink(lock_info->lock_path) < 0 && errno != ENOENT) {
			i_error("unlink(%s) failed: %m", lock_info->lock_path);
			return -1;
		}
		return 1;
	}

	if (lock_info->ino != st.st_ino ||
	    !CMP_DEV_T(lock_info->dev, st.st_dev) ||
	    lock_info->ctime != st.st_ctime ||
	    lock_info->mtime != st.st_mtime ||
	    lock_info->size != st.st_size) {
		/* either our first check or someone else got the lock file. */
		lock_info->dev = st.st_dev;
		lock_info->ino = st.st_ino;
		lock_info->ctime = st.st_ctime;
		lock_info->mtime = st.st_mtime;
		lock_info->size = st.st_size;

		pid = read_local_pid(lock_info->lock_path);
		lock_info->have_pid = pid != -1;
		lock_info->last_change = now;
	} else if (!lock_info->have_pid) {
		/* no pid checking */
		pid = -1;
	} else {
		if (lock_info->last_pid_check == now) {
			/* we just checked the pid */
			return 0;
		}

		/* re-read the pid. even if all times and inodes are the same,
		   the PID in the file might have changed if lock files were
		   rapidly being recreated. */
		pid = read_local_pid(lock_info->lock_path);
		lock_info->have_pid = pid != -1;
	}

	if (lock_info->have_pid) {
		/* we've local PID. Check if it exists. */
		if (kill(pid, 0) == 0 || errno != ESRCH) {
			if (pid != getpid())
				return 0;
			/* it's us. either we're locking it again, or it's a
			   stale lock file with same pid than us. either way,
			   recreate it.. */
		}

		/* doesn't exist - go ahead and delete */
		if (unlink(lock_info->lock_path) < 0 && errno != ENOENT) {
			i_error("unlink(%s) failed: %m", lock_info->lock_path);
			return -1;
		}
		return 1;
	}

	if (stale_timeout == 0) {
		/* no change checking */
		return 0;
	}

	if (lock_info->last_change != now) {
		if (stat(lock_info->path, &st) < 0) {
			if (errno == ENOENT) {
				/* file doesn't exist. treat it as if
				   it hasn't changed */
			} else {
				i_error("stat(%s) failed: %m", lock_info->path);
				return -1;
			}
		} else if (lock_info->last_size != st.st_size ||
			   lock_info->last_ctime != st.st_ctime ||
			   lock_info->last_mtime != st.st_mtime) {
			lock_info->last_change = now;
			lock_info->last_size = st.st_size;
			lock_info->last_ctime = st.st_ctime;
			lock_info->last_mtime = st.st_mtime;
		}
	}

	if (now > lock_info->last_change + stale_timeout) {
		/* no changes for a while, assume stale lock */
		if (unlink(lock_info->lock_path) < 0 && errno != ENOENT) {
			i_error("unlink(%s) failed: %m", lock_info->lock_path);
			return -1;
		}
		return 1;
	}

	return 0;
}

static int
create_temp_file(const char *prefix, const char **path_r, int write_pid)
{
	string_t *path;
	size_t len;
	struct stat st;
	const char *str;
	unsigned char randbuf[8];
	int fd;

	path = t_str_new(256);
	str_append(path, prefix);
	len = str_len(path);

	for (;;) {
		do {
			random_fill_weak(randbuf, sizeof(randbuf));
			str_truncate(path, len);
			str_append(path,
				   binary_to_hex(randbuf, sizeof(randbuf)));
			*path_r = str_c(path);
		} while (stat(*path_r, &st) == 0);

		if (errno != ENOENT) {
			i_error("stat(%s) failed: %m", *path_r);
			return -1;
		}

		fd = open(*path_r, O_RDWR | O_EXCL | O_CREAT, 0666);
		if (fd != -1)
			break;

		if (errno != EEXIST) {
			i_error("open(%s) failed: %m", *path_r);
			return -1;
		}
	}

	if (write_pid) {
		/* write our pid and host, if possible */
		str = t_strdup_printf("%s:%s", my_pid, my_hostname);
		if (write_full(fd, str, strlen(str)) < 0) {
			/* failed, leave it empty then */
			if (ftruncate(fd, 0) < 0) {
				i_error("ftruncate(%s) failed: %m", *path_r);
				(void)close(fd);
				return -1;
			}
		}
	}
	return fd;
}

static int try_create_lock(struct lock_info *lock_info, int write_pid)
{
	const char *temp_prefix = lock_info->set->temp_prefix;
	const char *str, *p;

	if (lock_info->temp_path == NULL) {
		/* we'll need our temp file first. */
		i_assert(lock_info->fd == -1);

		if (temp_prefix == NULL) {
			temp_prefix = t_strconcat(".temp.", my_hostname, ".",
						  my_pid, ".", NULL);
		}

		p = *temp_prefix == '/' ? NULL :
			strrchr(lock_info->lock_path, '/');
		if (p != NULL) {
			str = t_strdup_until(lock_info->lock_path, p+1);
			temp_prefix = t_strconcat(str, temp_prefix, NULL);
		}

		lock_info->fd = create_temp_file(temp_prefix, &str, write_pid);
		if (lock_info->fd == -1)
			return -1;

                lock_info->temp_path = str;
	}

	if (link(lock_info->temp_path, lock_info->lock_path) < 0) {
		if (errno == EEXIST)
			return 0;

		i_error("link(%s, %s) failed: %m",
			lock_info->temp_path, lock_info->lock_path);
		return -1;
	}

	if (unlink(lock_info->temp_path) < 0 && errno != ENOENT) {
		i_error("unlink(%s) failed: %m", lock_info->temp_path);
		/* non-fatal, continue */
	}
	lock_info->temp_path = NULL;

	return 1;
}

static int dotlock_create(const char *path, struct dotlock *dotlock,
			  enum dotlock_create_flags flags, int write_pid)
{
	const struct dotlock_settings *set = &dotlock->settings;
	const char *lock_path;
	struct lock_info lock_info;
	struct stat st;
	unsigned int stale_notify_threshold;
	unsigned int change_secs, wait_left;
	time_t now, max_wait_time, last_notify;
	int do_wait, ret;

	now = time(NULL);

	lock_path = t_strconcat(path, set->lock_suffix, NULL);
	stale_notify_threshold = set->stale_timeout / 2;
	max_wait_time = (flags & DOTLOCK_CREATE_FLAG_NONBLOCK) != 0 ? 0 :
		now + set->timeout;

	memset(&lock_info, 0, sizeof(lock_info));
	lock_info.path = path;
	lock_info.set = set;
	lock_info.lock_path = lock_path;
	lock_info.last_change = now;
	lock_info.fd = -1;

	last_notify = 0; do_wait = FALSE;

	do {
		if (do_wait) {
			usleep(LOCK_RANDOM_USLEEP_TIME);
			do_wait = FALSE;
		}

		ret = check_lock(now, &lock_info);
		if (ret < 0)
			break;

		if (ret == 1) {
			if ((flags & DOTLOCK_CREATE_FLAG_CHECKONLY) != 0)
				break;

			ret = try_create_lock(&lock_info, write_pid);
			if (ret != 0)
				break;
		}

		do_wait = TRUE;
		if (last_notify != now && set->callback != NULL) {
			last_notify = now;
			change_secs = now - lock_info.last_change;
			wait_left = max_wait_time - now;

			t_push();
			if (change_secs >= stale_notify_threshold &&
			    change_secs <= wait_left) {
				unsigned int secs_left =
					set->stale_timeout < change_secs ?
					0 : set->stale_timeout - change_secs;
				if (!set->callback(secs_left, TRUE,
						   set->context)) {
					/* we don't want to override */
					lock_info.last_change = now;
				}
			} else {
				(void)set->callback(wait_left, FALSE,
						    set->context);
			}
			t_pop();
		}

		now = time(NULL);
	} while (now < max_wait_time);

	if (ret > 0) {
		if (fstat(lock_info.fd, &st) < 0) {
			i_error("fstat(%s) failed: %m", lock_path);
			ret = -1;
		} else {
			dotlock->dev = st.st_dev;
			dotlock->ino = st.st_ino;

			dotlock->path = i_strdup(path);
			dotlock->fd = lock_info.fd;
			lock_info.fd = -1;
		}
	}

	if (lock_info.fd != -1) {
		int old_errno = errno;

		if (close(lock_info.fd) < 0)
			i_error("close(%s) failed: %m", lock_path);
		errno = old_errno;
	}

	if (ret == 0)
		errno = EAGAIN;
	return ret;
}

static void file_dotlock_free(struct dotlock *dotlock)
{
	int old_errno;

	if (dotlock->fd != -1) {
		old_errno = errno;
		if (close(dotlock->fd) < 0)
			i_error("close(%s) failed: %m", dotlock->path);
		dotlock->fd = -1;
		errno = old_errno;
	}

	i_free(dotlock->path);
	i_free(dotlock);
}

int file_dotlock_create(const struct dotlock_settings *set, const char *path,
			enum dotlock_create_flags flags,
			struct dotlock **dotlock_r)
{
	struct dotlock *dotlock;
	const char *lock_path;
	struct stat st;
	int fd, ret;

	*dotlock_r = NULL;

	dotlock = file_dotlock_alloc(set);
	lock_path = t_strconcat(path, dotlock->settings.lock_suffix, NULL);

	ret = dotlock_create(path, dotlock, flags, TRUE);
	if (ret <= 0 || (flags & DOTLOCK_CREATE_FLAG_CHECKONLY) != 0) {
		i_free(dotlock);
		return ret;
	}

	fd = dotlock->fd;
	dotlock->fd = -1;

	if (close(fd) < 0) {
		i_error("close(%s) failed: %m", lock_path);
		file_dotlock_free(dotlock);
		return -1;
	}

	/* some NFS implementations may have used cached mtime in previous
	   fstat() call. Check again to avoid "dotlock was modified" errors. */
	if (stat(lock_path, &st) < 0) {
		i_error("stat(%s) failed: %m", lock_path);
                file_dotlock_free(dotlock);
		return -1;
	}
	/* extra sanity check won't hurt.. */
	if (st.st_dev != dotlock->dev || st.st_ino != dotlock->ino) {
		i_error("dotlock %s was immediately recreated under us",
			lock_path);
                file_dotlock_free(dotlock);
		return -1;
	}
	dotlock->mtime = st.st_mtime;

	*dotlock_r = dotlock;
	return 1;
}

int file_dotlock_delete(struct dotlock **dotlock_p)
{
	struct dotlock *dotlock;
	const char *lock_path;
        struct stat st;

	dotlock = *dotlock_p;
	*dotlock_p = NULL;

	lock_path = t_strconcat(dotlock->path,
				dotlock->settings.lock_suffix, NULL);

	if (lstat(lock_path, &st) < 0) {
		if (errno == ENOENT) {
			i_warning("Our dotlock file %s was deleted", lock_path);
			file_dotlock_free(dotlock);
			return 0;
		}

		i_error("lstat(%s) failed: %m", lock_path);
		file_dotlock_free(dotlock);
		return -1;
	}

	if (dotlock->ino != st.st_ino ||
	    !CMP_DEV_T(dotlock->dev, st.st_dev)) {
		i_warning("Our dotlock file %s was overridden", lock_path);
		errno = EEXIST;
		file_dotlock_free(dotlock);
		return 0;
	}

	if (dotlock->mtime != st.st_mtime && dotlock->fd == -1) {
		i_warning("Our dotlock file %s was modified (%s vs %s), "
			  "assuming it wasn't overridden", lock_path,
			  dec2str(dotlock->mtime), dec2str(st.st_mtime));
	}

	if (unlink(lock_path) < 0) {
		if (errno == ENOENT) {
			i_warning("Our dotlock file %s was deleted", lock_path);
			file_dotlock_free(dotlock);
			return 0;
		}

		i_error("unlink(%s) failed: %m", lock_path);
		file_dotlock_free(dotlock);
		return -1;
	}

	file_dotlock_free(dotlock);
	return 1;
}

int file_dotlock_open(const struct dotlock_settings *set, const char *path,
		      enum dotlock_create_flags flags,
		      struct dotlock **dotlock_r)
{
	struct dotlock *dotlock;
	int ret;

	dotlock = file_dotlock_alloc(set);

	ret = dotlock_create(path, dotlock, flags, FALSE);
	if (ret <= 0) {
		file_dotlock_free(dotlock);
		*dotlock_r = NULL;
		return -1;
	}

	*dotlock_r = dotlock;
	return dotlock->fd;
}

int file_dotlock_replace(struct dotlock **dotlock_p,
			 enum dotlock_replace_flags flags)
{
	struct dotlock *dotlock;
	struct stat st, st2;
	const char *lock_path;
	int fd;

	dotlock = *dotlock_p;
	*dotlock_p = NULL;

	fd = dotlock->fd;
	if ((flags & DOTLOCK_REPLACE_FLAG_DONT_CLOSE_FD) != 0)
		dotlock->fd = -1;

	lock_path = t_strconcat(dotlock->path,
				dotlock->settings.lock_suffix, NULL);
	if ((flags & DOTLOCK_REPLACE_FLAG_VERIFY_OWNER) != 0) {
		if (fstat(fd, &st) < 0) {
			i_error("fstat(%s) failed: %m", lock_path);
			file_dotlock_free(dotlock);
			return -1;
		}

		if (lstat(lock_path, &st2) < 0) {
			i_error("lstat(%s) failed: %m", lock_path);
			file_dotlock_free(dotlock);
			return -1;
		}

		if (st.st_ino != st2.st_ino ||
		    !CMP_DEV_T(st.st_dev, st2.st_dev)) {
			i_warning("Our dotlock file %s was overridden",
				  lock_path);
			errno = EEXIST;
			file_dotlock_free(dotlock);
			return 0;
		}
	}

	if (rename(lock_path, dotlock->path) < 0) {
		i_error("rename(%s, %s) failed: %m", lock_path, dotlock->path);
		file_dotlock_free(dotlock);
		return -1;
	}
	file_dotlock_free(dotlock);
	return 1;
}
