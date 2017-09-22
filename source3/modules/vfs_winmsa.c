/*
 *	Windows MoveSecurityAttributes
 */

#include "includes.h"
#include "system/filesys.h"
#include "smbd/smbd.h"
#include "nfs4_acls.h"

#if HAVE_FREEBSD_SUNACL_H
#include "sunacl.h"
#endif

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_VFS

#define WINMSA_MODULE_NAME "winmsa"

typedef struct winmsa_info {
	int d_naces;
	int f_naces;
	ace_t *d_aces;
	ace_t *f_aces;
	uid_t uid;
	gid_t gid;
	char *path;
} winmsa_info_t;


static char *parent_dir(TALLOC_CTX *ctx, const char *name)
{
	const char *p = strrchr(name, '/');
	if (p == NULL)
		return NULL;

	return  talloc_strndup(ctx, name, (p + 1) - name);
}

static void winmsa_dump_acl(const char *path, ace_t *aces, int naces)
{
	int i;
	ace_t *ace;

	DEBUG(5, ("PATH=%s\n", path));
	for (i = 0;i < naces;i++) {
		ace = &(aces[i]);
		DEBUG(5, ("ACE: [%02d/%02d] who=%08x [%-10d] mask=%08x flags=%08x type=%08x\n",
			i + 1, naces, ace->a_who, ace->a_who, ace->a_access_mask, ace->a_flags, ace->a_type));
	}
}

static int winmsa_get_naces(const char *path)
{
	int naces;

	if ((naces = acl(path, ACE_GETACLCNT, 0, NULL)) < 0) {
		if(errno == ENOSYS) {
			DEBUG(5, ("acl(ACE_GETACLCNT, %s): Operation is not "
				"supported on the filesystem where the file reside\n", path));
		} else {
			DEBUG(5, ("acl(ACE_GETACLCNT, %s): %s ", path, strerror(errno)));
		}
		return -1;
	}

	return naces;
}

static int winmsa_get_acl(TALLOC_CTX *ctx, winmsa_info_t *info)
{
	if (info == NULL || info->path == NULL)
		return -1;

	if ((info->d_naces = winmsa_get_naces(info->path)) < 0)
		return -1;

	if ((info->d_aces = talloc_size(ctx, sizeof(ace_t) * info->d_naces)) == NULL) {
		errno = ENOMEM;
		return -1;
	}

	if ((acl(info->path, ACE_GETACL, info->d_naces, info->d_aces)) < 0) {
		DEBUG(3, ("winmsa_get_acl(%s): %s ", info->path, strerror(errno)));
		return -1;
	}

	return 0;
}

static int winmsa_file_acl(TALLOC_CTX *ctx, winmsa_info_t *info)	
{
	int i;

	if (info == NULL || info->path == NULL)
		return -1;

	if ((info->f_naces = winmsa_get_naces(info->path)) < 0)
		return -1;

	if ((info->f_aces = talloc_size(ctx, sizeof(ace_t) * info->f_naces)) == NULL) {
		errno = ENOMEM;
		return -1;
	}

	if ((acl(info->path, ACE_GETACL, info->f_naces, info->f_aces)) < 0) {
		DEBUG(3, ("winmsa_file_acl(%s): %s ", info->path, strerror(errno)));
		return -1;
	}

	for (i = 0;i < info->f_naces;i++) {
		info->f_aces[i].a_flags &= ~(
			ACE_FILE_INHERIT_ACE|
			ACE_DIRECTORY_INHERIT_ACE|
			ACE_NO_PROPAGATE_INHERIT_ACE|
			ACE_INHERIT_ONLY_ACE
		);
	}

	return 0;
}

static int winmsa_get_ownership(winmsa_info_t *info)	
{
	SMB_STRUCT_STAT sbuf;

	if (sys_lstat(info->path, &sbuf, false) < 0) {
		DEBUG(3, ("winmsa_get_ownership: stat failed for %s\n", info->path));
		return -1;
	}

	info->uid = sbuf.st_ex_uid;
	info->gid = sbuf.st_ex_gid;

	return 0;
}

static int winmsa_set_acls(TALLOC_CTX *ctx, struct vfs_handle_struct *handle,
						winmsa_info_t *info, const char *path)
{
	DIR *dh;
	struct dirent *de;
	SMB_STRUCT_STAT sbuf;

	if (sys_lstat(path, &sbuf, false) < 0) {
		DEBUG(3, ("winmsa_set_acls: stat failed for %s\n", path));
		return -1;
	}

	if (S_ISLNK(sbuf.st_ex_mode))
		return 0;

	if (!S_ISDIR(sbuf.st_ex_mode)) {
		if (chown(path, info->uid, info->gid) < 0)
			DEBUG(3, ("winmsa_set_acls: chown failed for %s\n", path));
		if (acl(path, ACE_SETACL, info->f_naces, info->f_aces) < 0)
			DEBUG(3, ("winmsa_set_acls: acl failed for %s\n", path));
		return 0;
	}

	if ((dh = opendir(path)) == NULL) {
		DEBUG(3, ("winmsa_set_acls: opendir failed for %s\n", path));
		return -1;
	}

	while ((de = readdir(dh)) != NULL) {
		char *rp, *buf;

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0) {
			continue;
		}

		if ((rp = talloc_size(ctx, PATH_MAX)) == NULL) {
			errno = ENOMEM;
			return -1;
		}

		if ((buf = talloc_size(ctx, PATH_MAX)) == NULL) {
			talloc_free(rp);
			errno = ENOMEM;
			return -1;
		}

		if (realpath(path, rp) == NULL) {
			talloc_free(buf);
			talloc_free(rp);
			DEBUG(3, ("winmsa_set_acls: realpath failed for %s\n", path));
			continue;
		}

		snprintf(buf, PATH_MAX, "%s/%s", rp, de->d_name);
		talloc_free(rp);

		winmsa_set_acls(ctx, handle, info, buf);
		talloc_free(buf);
	}

	closedir(dh);

	if (chown(path, info->uid, info->gid) < 0)
		DEBUG(3, ("winmsa_set_acls: chown failed for %s\n", path));
	if (acl(path, ACE_SETACL, info->d_naces, info->d_aces) < 0)
		DEBUG(3, ("winmsa_set_acls: acl failed for %s\n", path));

	return 0;
}

static int winmsa_rename(struct vfs_handle_struct *handle,
						const struct smb_filename *smb_fname_src,
						const struct smb_filename *smb_fname_dst)
{

	int result = -1;
	winmsa_info_t *info;
	char *parent, *dst;
	TALLOC_CTX *ctx;


	if (SMB_VFS_NEXT_RENAME(handle, smb_fname_src, smb_fname_dst) < 0) {
		DEBUG(3, ("winmsa_rename: rename failed: %s\n", strerror(errno)));
		result = -1;
		goto out;
	}

	if ((ctx = talloc_new(NULL)) == NULL) {
		DEBUG(3, ("winmsa_rename: talloc failed\n"));
		result = -1;
		goto out;
	}

	if ((parent = parent_dir(ctx, smb_fname_dst->base_name)) == NULL) {
		result = 0;
		goto out;
	}

	if ((info = talloc_size(ctx, sizeof(winmsa_info_t))) == NULL) {
		DEBUG(3, ("winmsa_rename: talloc_size failed\n"));
		result = -1;
		goto out;
	}

	info->path = talloc_size(ctx, PATH_MAX);
	if (realpath(parent, info->path) == NULL) {
		DEBUG(3, ("winmsa_rename: realpath failed for %s\n", parent));
		result = -1;
		goto out;
	}

	dst = talloc_size(ctx, PATH_MAX);
	if (realpath(smb_fname_dst->base_name, dst) == NULL) {
		DEBUG(3, ("winmsa_rename: realpath failed for %s\n", smb_fname_dst->base_name));
		result = -1;
		goto out;
	}

	if (winmsa_get_acl(ctx, info) < 0) {
		DEBUG(3, ("winmsa_rename: winmsa_get_acl failed\n"));
		result = -1;
		goto out;
	}

	if (winmsa_file_acl(ctx, info) < 0) {
		DEBUG(3, ("winmsa_rename: winmsa_file_acl failed\n"));
		result = -1;
		goto out;
	}

	if (winmsa_get_ownership(info) < 0) {
		DEBUG(3, ("winmsa_rename: winmsa_get_ownership failed\n"));
		result = -1;
		goto out;
	}

	if ((result = winmsa_set_acls(ctx, handle, info, dst)) < 0) {
		DEBUG(3, ("winmsa_rename: winmsa_set_acls failed\n"));
		result = -1;
	}

out:
	TALLOC_FREE(ctx);
	return result;
}

static struct vfs_fn_pointers winmsa_fns = {
	.rename_fn = winmsa_rename,
};

NTSTATUS vfs_winmsa_init(void);
NTSTATUS vfs_winmsa_init(void)
{
	return smb_register_vfs(SMB_VFS_INTERFACE_VERSION, "winmsa",
				&winmsa_fns);
}
