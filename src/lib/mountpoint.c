/* Copyright (c) 2006 Timo Sirainen */

#include "lib.h"
#include "mountpoint.h"

#include <sys/stat.h>

#ifdef HAVE_STATVFS_MNTFROMNAME
#  include <sys/statvfs.h> /* NetBSD 3.0+, FreeBSD 5.0+ */
#  define STATVFS_STR "statvfs"
#elif HAVE_STATFS_MNTFROMNAME
#  include <sys/param.h> /* Older BSDs */
#  include <sys/mount.h>
#  define statvfs statfs
#  define STATVFS_STR "statfs"
#elif defined(HAVE_MNTENT_H)
#  include <stdio.h>
#  include <mntent.h> /* Linux */
#elif defined(HAVE_SYS_MNTTAB_H)
#  include <stdio.h>
#  include <sys/mnttab.h> /* Solaris */
#else
#  define MOUNTPOINT_UNKNOWN
#endif

#ifdef HAVE_SYS_MNTTAB_H
#  define MTAB_PATH MNTTAB /* Solaris */
#else
#  define MTAB_PATH "/etc/mtab" /* Linux */
#endif

/* AIX doesn't have these defined */
#ifndef MNTTYPE_SWAP
#  define MNTTYPE_SWAP "swap"
#endif
#ifndef MNTTYPE_IGNORE
#  define MNTTYPE_IGNORE "ignore"
#endif

int mountpoint_get(const char *path, pool_t pool, struct mountpoint *point_r)
{
#ifdef MOUNTPOINT_UNKNOWN
	memset(point_r, 0, sizeof(*point_r));
	errno = ENOSYS;
	return -1;
#elif defined (HAVE_STATFS_MNTFROMNAME) || defined(HAVE_STATVFS_MNTFROMNAME)
	/* BSDs */
	struct statvfs buf;

	memset(point_r, 0, sizeof(*point_r));
	if (statvfs(path, &buf) < 0) {
		if (errno == ENOENT)
			return 0;

		i_error(STATVFS_STR"(%s) failed: %m", path);
		return -1;
	}

	point_r->device_path = p_strdup(pool, buf.f_mntfromname);
	point_r->mount_path = p_strdup(pool, buf.f_mntonname);
	point_r->type = p_strdup(pool, buf.f_fstypename);
	point_r->block_size = buf.f_bsize;
	return 1;
#else
	/* Linux, Solaris: /etc/mtab reading */
#ifdef HAVE_SYS_MNTTAB_H
	struct mnttab ent;
#else
	struct mntent *ent;
#endif
	struct stat st, st2;
	const char *device_path = NULL, *mount_path = NULL, *type = NULL;
	unsigned int block_size;
	FILE *f;

	memset(point_r, 0, sizeof(*point_r));
	if (stat(path, &st) < 0) {
		if (errno == ENOENT)
			return 0;

		i_error("stat(%s) failed: %m", path);
		return -1;
	}
	block_size = st.st_blksize;

#ifdef HAVE_SYS_MNTTAB_H
	/* Solaris */
	f = fopen(MTAB_PATH, "r");
	if (f == NULL) {
		i_error("fopen(%s) failed: %m", MTAB_PATH);
		return -1;
	}
	while ((getmntent(f, &ent)) == 0) {
		if (strcmp(ent.mnt_fstype, MNTTYPE_SWAP) == 0 ||
		    strcmp(ent.mnt_fstype, MNTTYPE_IGNORE) == 0)
			continue;

		if (stat(ent.mnt_mountp, &st2) == 0 &&
		    CMP_DEV_T(st.st_dev, st2.st_dev)) {
			device_path = ent.mnt_special;
			mount_path = ent.mnt_mountp;
			type = ent.mnt_fstype;
			break;
		}
	}
	fclose(f);
#else
	/* Linux */
	f = setmntent(MTAB_PATH, "r");
	if (f == NULL) {
		i_error("setmntent(%s) failed: %m", MTAB_PATH);
		return -1;
	}
	while ((ent = getmntent(f)) != NULL) {
		if (strcmp(ent->mnt_type, MNTTYPE_SWAP) == 0 ||
		    strcmp(ent->mnt_type, MNTTYPE_IGNORE) == 0)
			continue;

		if (stat(ent->mnt_dir, &st2) == 0 &&
		    CMP_DEV_T(st.st_dev, st2.st_dev)) {
			device_path = ent->mnt_fsname;
			mount_path = ent->mnt_dir;
			type = ent->mnt_type;
			break;
		}
	}
	endmntent(f);
#endif
	if (device_path == NULL)
		return 0;

	point_r->device_path = p_strdup(pool, device_path);
	point_r->mount_path = p_strdup(pool, mount_path);
	point_r->type = p_strdup(pool, type);
	point_r->block_size = block_size;
	return 1;
#endif
}
