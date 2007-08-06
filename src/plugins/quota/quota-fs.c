/* Copyright (C) 2005-2006 Timo Sirainen */

/* Only for reporting filesystem quota */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "mountpoint.h"
#include "quota-private.h"
#include "quota-fs.h"

#ifdef HAVE_FS_QUOTA

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#ifdef HAVE_LINUX_DQBLK_XFS_H
#  include <linux/dqblk_xfs.h>
#  define HAVE_XFS_QUOTA
#elif defined (HAVE_XFS_XQM_H)
#  include <xfs/xqm.h> /* CentOS 4.x at least uses this */
#  define HAVE_XFS_QUOTA
#endif

#ifdef HAVE_RQUOTA
#  include "rquota_xdr.c"
#  define RQUOTA_GETQUOTA_TIMEOUT_SECS 10
#endif

#ifndef DEV_BSIZE
#  define DEV_BSIZE 512
#endif

#ifdef HAVE_STRUCT_DQBLK_CURSPACE
#  define dqb_curblocks dqb_curspace
#endif

/* Older sys/quota.h doesn't define _LINUX_QUOTA_VERSION at all, which means
   it supports only v1 quota */
#ifndef _LINUX_QUOTA_VERSION
#  define _LINUX_QUOTA_VERSION 1
#endif

struct fs_quota_mountpoint {
	char *mount_path;
	char *device_path;
	char *type;

#ifdef HAVE_Q_QUOTACTL
	int fd;
	char *path;
#endif
};

struct fs_quota_root {
	struct quota_root root;

	uid_t uid;
	struct fs_quota_mountpoint *mount;
};

extern struct quota_backend quota_backend_fs;

static struct quota_root *fs_quota_alloc(void)
{
	struct fs_quota_root *root;

	root = i_new(struct fs_quota_root, 1);
	root->uid = geteuid();

	return &root->root;
}

static void fs_quota_mountpoint_free(struct fs_quota_mountpoint *mount)
{
#ifdef HAVE_Q_QUOTACTL
	if (mount->fd != -1) {
		if (close(mount->fd) < 0)
			i_error("close(%s) failed: %m", mount->path);
	}
	i_free(mount->path);
#endif

	i_free(mount->device_path);
	i_free(mount->mount_path);
	i_free(mount->type);
	i_free(mount);
}

static void fs_quota_deinit(struct quota_root *_root)
{
	struct fs_quota_root *root = (struct fs_quota_root *)_root;

	if (root->mount != NULL)
		fs_quota_mountpoint_free(root->mount);
	i_free(root);
}

static struct fs_quota_mountpoint *fs_quota_mountpoint_get(const char *dir)
{
	struct fs_quota_mountpoint *mount;
	struct mountpoint point;
	int ret;

	ret = mountpoint_get(dir, default_pool, &point);
	if (ret <= 0)
		return NULL;

	mount = i_new(struct fs_quota_mountpoint, 1);
	mount->device_path = point.device_path;
	mount->mount_path = point.mount_path;
	mount->type = point.type;
	return mount;
}

static struct fs_quota_root *
fs_quota_root_find_mountpoint(struct quota *quota,
			      const struct fs_quota_mountpoint *mount)
{
	struct quota_root *const *roots;
	struct fs_quota_root *empty = NULL;
	unsigned int i, count;

	roots = array_get(&quota->roots, &count);
	for (i = 0; i < count; i++) {
		if (roots[i]->backend.name == quota_backend_fs.name) {
			struct fs_quota_root *root =
				(struct fs_quota_root *)roots[i];

			if (root->mount == NULL)
				empty = root;
			else if (strcmp(root->mount->mount_path,
					mount->mount_path) == 0)
				return root;
		}
	}
	return empty;
}

static void fs_quota_storage_added(struct quota *quota,
				   struct mail_storage *storage)
{
	struct fs_quota_mountpoint *mount;
	struct quota_root *_root;
	struct fs_quota_root *root;
	const char *dir;
	bool is_file;

	dir = mail_storage_get_mailbox_path(storage, "", &is_file);
	mount = fs_quota_mountpoint_get(dir);
	if (getenv("DEBUG") != NULL) {
		i_info("fs quota add storage dir = %s", dir);
		i_info("fs quota block device = %s", mount->device_path);
		i_info("fs quota mount point = %s", mount->mount_path);
	}

	root = fs_quota_root_find_mountpoint(quota, mount);
	if (root != NULL && root->mount != NULL) {
		/* already exists */
		fs_quota_mountpoint_free(mount);
		return;
	}

	if (root == NULL) {
		/* create a new root for this mountpoint */
		_root = quota_root_init(quota, quota_backend_fs.name);
		root = (struct fs_quota_root *)_root;
		root->root.name =
			p_strdup_printf(root->root.pool, "%s%d",
					quota_backend_fs.name,
					array_count(&quota->roots));
	} else {
		/* this is the default root. */
	}
	root->mount = mount;

#ifdef HAVE_Q_QUOTACTL
	if (mount->path == NULL) {
		mount->path = i_strconcat(mount->mount_path, "/quotas", NULL);
		mount->fd = open(mount->path, O_RDONLY);
		if (mount->fd == -1 && errno != ENOENT)
			i_error("open(%s) failed: %m", mount->path);
	}
#endif
}

static const char *const *
fs_quota_root_get_resources(struct quota_root *root __attr_unused__)
{
	static const char *resources[] = { QUOTA_NAME_STORAGE_KILOBYTES, NULL };

	return resources;
}

#ifdef HAVE_RQUOTA
/* retrieve user quota from a remote host */
static int do_rquota(struct fs_quota_root *root, uint64_t *value_r,
		     uint64_t *limit_r)
{
	struct getquota_rslt result;
	struct getquota_args args;
	struct timeval timeout;
	enum clnt_stat call_status;
	CLIENT *cl;
	struct fs_quota_mountpoint *mount = root->mount;
	const char *host;
	char *path;

	path = strchr(mount->device_path, ':');
	if (path == NULL) {
		i_error("quota-fs: %s is not a valid NFS device path",
			mount->device_path);
		return -1;
	}

	host = t_strdup_until(mount->device_path, path);
	path++;

	if (getenv("DEBUG") != NULL) {
		i_info("quota-fs: host=%s, path=%s, uid=%s",
			host, path, dec2str(root->uid));
	}

	/* clnt_create() polls for a while to establish a connection */
	cl = clnt_create(host, RQUOTAPROG, RQUOTAVERS, "udp");
	if (cl == NULL) {
		i_error("quota-fs: could not contact RPC service on %s",
			host);
		return -1;
	}

	/* Establish some RPC credentials */
	auth_destroy(cl->cl_auth);
	cl->cl_auth = authunix_create_default();

	/* make the rquota call on the remote host */
	args.gqa_pathp = path;
	args.gqa_uid = root->uid;

	timeout.tv_sec = RQUOTA_GETQUOTA_TIMEOUT_SECS;
	timeout.tv_usec = 0;
	call_status = clnt_call(cl, RQUOTAPROC_GETQUOTA,
				(xdrproc_t)xdr_getquota_args, (char *)&args,
				(xdrproc_t)xdr_getquota_rslt, (char *)&result,
				timeout);
	
	/* the result has been deserialized, let the client go */
	auth_destroy(cl->cl_auth);
	clnt_destroy(cl);

	if (call_status != RPC_SUCCESS) {
		const char *rpc_error_msg = clnt_sperrno(call_status);

		i_error("quota-fs: remote rquota call failed: %s",
			rpc_error_msg);
		return -1;
	}

	switch (result.status) {
	case Q_OK: {
		/* convert the results from blocks to bytes */
		rquota *rq = &result.getquota_rslt_u.gqr_rquota;

		if (rq->rq_active) {
			*value_r = (uint64_t)rq->rq_curblocks *
				(uint64_t)rq->rq_bsize;
			*limit_r = (uint64_t)rq->rq_bsoftlimit *
				(uint64_t)rq->rq_bsize;
		}
		if (getenv("DEBUG") != NULL) {
			i_info("quota-fs: uid=%s, value=%llu, "
			       "limit=%llu, active=%d", dec2str(root->uid),
			       (unsigned long long)*value_r,
			       (unsigned long long)*limit_r, rq->rq_active);
		}
		return 1;
	}
	case Q_NOQUOTA:
		if (getenv("DEBUG") != NULL) {
			i_info("quota-fs: uid=%s, limit=unlimited",
			       dec2str(root->uid));
		}
		return 1;
	case Q_EPERM:
		i_error("quota-fs: permission denied to rquota service");
		return -1;
	default:
		i_error("quota-fs: unrecognized status code (%d) "
			"from rquota service", result.status);
		return -1;
	}
}
#endif

static int
fs_quota_get_resource(struct quota_root *_root, const char *name,
		      uint64_t *value_r, uint64_t *limit_r)
{
	struct fs_quota_root *root = (struct fs_quota_root *)_root;
	struct dqblk dqblk;
#ifdef HAVE_Q_QUOTACTL
	struct quotctl ctl;
#endif

	*value_r = 0;
	*limit_r = 0;

	if (strcasecmp(name, QUOTA_NAME_STORAGE_BYTES) != 0 ||
	    root->mount == NULL)
		return 0;

#ifdef HAVE_RQUOTA
	if (strcmp(root->mount->type, "nfs") == 0) {
		int ret;

		t_push();
		ret = do_rquota(root, value_r, limit_r);
		t_pop();
		return ret;
	}
#endif

#if defined (HAVE_QUOTACTL) && defined(HAVE_SYS_QUOTA_H)
	/* Linux */
#ifdef HAVE_XFS_QUOTA
	if (strcmp(root->mount->type, "xfs") == 0) {
		/* XFS */
		struct fs_disk_quota xdqblk;

		if (quotactl(QCMD(Q_XGETQUOTA, USRQUOTA),
			     root->mount->device_path,
			     root->uid, (caddr_t)&xdqblk) < 0) {
			i_error("quotactl(Q_XGETQUOTA, %s) failed: %m",
				root->mount->device_path);
			return -1;
		}

		/* values always returned in 512 byte blocks */
		*value_r = xdqblk.d_bcount * 512;
		*limit_r = xdqblk.d_blk_softlimit * 512;
	} else
#endif
	{
		/* ext2, ext3 */
		if (quotactl(QCMD(Q_GETQUOTA, USRQUOTA),
			     root->mount->device_path,
			     root->uid, (caddr_t)&dqblk) < 0) {
			i_error("quotactl(Q_GETQUOTA, %s) failed: %m",
				root->mount->device_path);
			if (errno == EINVAL) {
				i_error("Dovecot was compiled with Linux quota "
					"v%d support, try changing it "
					"(--with-linux-quota configure option)",
					_LINUX_QUOTA_VERSION);
			}
			return -1;
		}

#if _LINUX_QUOTA_VERSION == 1
		*value_r = dqblk.dqb_curblocks * 1024;
#else
		*value_r = dqblk.dqb_curblocks;
#endif
		*limit_r = dqblk.dqb_bsoftlimit * 1024;
	}
#elif defined(HAVE_QUOTACTL)
	/* BSD, AIX */
	if (quotactl(root->mount->mount_path, QCMD(Q_GETQUOTA, USRQUOTA),
		     root->uid, (void *)&dqblk) < 0) {
		i_error("quotactl(Q_GETQUOTA, %s) failed: %m",
			root->mount->mount_path);
		return -1;
	}
	*value_r = (uint64_t)dqblk.dqb_curblocks * DEV_BSIZE;
	*limit_r = (uint64_t)dqblk.dqb_bsoftlimit * DEV_BSIZE;
#else
	/* Solaris */
	if (root->mount->fd == -1)
		return 0;

	ctl.op = Q_GETQUOTA;
	ctl.uid = root->uid;
	ctl.addr = (caddr_t)&dqblk;
	if (ioctl(root->mount->fd, Q_QUOTACTL, &ctl) < 0) {
		i_error("ioctl(%s, Q_QUOTACTL) failed: %m", root->mount->path);
		return -1;
	}
	*value_r = (uint64_t)dqblk.dqb_curblocks * DEV_BSIZE;
	*limit_r = (uint64_t)dqblk.dqb_bsoftlimit * DEV_BSIZE;
#endif
	return 1;
}

static int 
fs_quota_update(struct quota_root *root __attr_unused__,
		struct quota_transaction_context *ctx __attr_unused__)
{
	return 0;
}

struct quota_backend quota_backend_fs = {
	"fs",

	{
		fs_quota_alloc,
		NULL,
		fs_quota_deinit,
		NULL,

		fs_quota_storage_added,

		fs_quota_root_get_resources,
		fs_quota_get_resource,
		fs_quota_update
	}
};

#endif
