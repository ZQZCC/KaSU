// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 \xx
 *
 * This file is a downstream extension and NOT affiliated, endorsed by,
 * or maintained by the official KernelSU developers.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/workqueue.h>

#ifdef CONFIG_KSU_TINYFS_PKG_OBSERVER

#if defined(MODULE) || LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0) || \
	LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
#error "TinyFS package observer requires a built-in Linux 6.1 kernel"
#endif

static DEFINE_MUTEX(ksu_pkg_observer_mutex);
static struct path ksu_pkg_system_path;
static const struct inode_operations *ksu_pkg_orig_iops;
static struct inode_operations ksu_pkg_iops;

static void ksu_pkg_prune(struct work_struct *work)
{
	struct inode *dir = d_inode(ksu_pkg_system_path.dentry);

	/* The filesystem callback precedes VFS d_move; wait for rename's unlock. */
	inode_lock_shared(dir);
	inode_unlock_shared(dir);
	ksu_prune_allowlist(&ksu_pkg_system_path);
}

static DECLARE_WORK(ksu_pkg_prune_work, ksu_pkg_prune);

static int ksu_pkg_rename(struct user_namespace *mnt_userns,
			  struct inode *old_dir, struct dentry *old_dentry,
			  struct inode *new_dir, struct dentry *new_dentry,
			  unsigned int flags)
{
	static const char plist[] = "packages.list";
	/* Capture the destination before filesystems can move the dentries. */
	bool observe = current->mm && current_uid().val == 1000 &&
		new_dir == d_inode(ksu_pkg_system_path.dentry) &&
		new_dentry->d_name.len == sizeof(plist) - 1 &&
		!memcmp_inline(new_dentry->d_name.name, plist, sizeof(plist) - 1);
	int ret = ksu_pkg_orig_iops->rename(mnt_userns, old_dir, old_dentry,
					new_dir, new_dentry, flags);

	if (!ret && observe)
		schedule_work(&ksu_pkg_prune_work);
	return ret;
}

static bool ksu_pkg_observer_init(void)
{
	struct path path;
	struct inode *dir;
	const struct inode_operations *iops;
	int ret = 0;

	mutex_lock(&ksu_pkg_observer_mutex);
	if (ksu_pkg_orig_iops)
		goto out;

	ret = kern_path("/data/system", LOOKUP_FOLLOW, &path);
	if (ret)
		goto failed;

	dir = d_inode(path.dentry);
	if (!dir || !S_ISDIR(dir->i_mode)) {
		ret = -ENOTDIR;
		goto put_path;
	}

	inode_lock(dir);
	iops = READ_ONCE(dir->i_op);
	if (!iops || !iops->rename) {
		inode_unlock(dir);
		ret = -EOPNOTSUPP;
		goto put_path;
	}

	ksu_pkg_iops = *iops;
	ksu_pkg_iops.rename = ksu_pkg_rename;
	ksu_pkg_orig_iops = iops;
	/* Built-in only: retain the path for the lifetime of the proxy. */
	ksu_pkg_system_path = path;
	smp_store_release(&dir->i_op, &ksu_pkg_iops);
	inode_unlock(dir);
	pr_info("pkg_observer: watching /data/system\n");
	goto out;

put_path:
	path_put(&path);
failed:
	pr_err("pkg_observer: cannot watch /data/system: %d\n", ret);
out:
	mutex_unlock(&ksu_pkg_observer_mutex);
	return !ret;
}

#endif
