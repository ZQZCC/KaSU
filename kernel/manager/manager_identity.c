// SPDX-License-Identifier: GPL-2.0-only
#include <linux/fs_struct.h>

static struct path ksu_manager_root;
static bool ksu_manager_root_ready;

void ksu_manager_identity_init(void)
{
	/* Capture init's root, not an application's isolated /data view. */
	get_fs_root(current->fs, &ksu_manager_root);
	smp_store_release(&ksu_manager_root_ready, true);
}

static struct file *ksu_open_manager_directory(uid_t uid)
{
	char name[sizeof("/data/user_de/") + 10 + sizeof(KSU_PACKAGE_NAME)];
	const struct cred *old_cred;
	struct file *file;

	if (!smp_load_acquire(&ksu_manager_root_ready))
		return ERR_PTR(-ENOENT);

	snprintf(name, sizeof(name), "/data/user_de/%u/" KSU_PACKAGE_NAME,
		 uid / KSU_PER_USER_RANGE);
	old_cred = override_creds(ksu_cred);
	file = file_open_root(&ksu_manager_root, name,
			      O_PATH | O_DIRECTORY | O_NOFOLLOW, 0);
	revert_creds(old_cred);
	return file;
}

bool is_uid_manager(uid_t uid)
{
	struct file *file;
	bool manager;

	if (!uid)
		return false;
	file = ksu_open_manager_directory(uid);
	if (IS_ERR(file))
		return false;
	manager = uid_eq(file_inode(file)->i_uid, KUIDT_INIT(uid));
	fput(file);
	return manager;
}

uid_t ksu_get_manager_appid(void)
{
	struct file *file = ksu_open_manager_directory(current_uid().val);
	uid_t appid;

	if (IS_ERR(file))
		return KSU_INVALID_APPID;
	appid = from_kuid(&init_user_ns, file_inode(file)->i_uid) % KSU_PER_USER_RANGE;
	fput(file);
	return appid;
}
