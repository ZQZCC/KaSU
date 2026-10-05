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

#ifdef CONFIG_KSU_TINYFS_PKG_OBSERVER

#if defined(MODULE) || LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 0) || \
	LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
#error "TinyFS package observer requires a built-in Linux 6.1 kernel"
#endif

static DEFINE_MUTEX(ksu_pkg_observer_mutex);
static struct path ksu_pkg_system_path;
static const struct inode_operations *ksu_pkg_orig_iops;
static struct inode_operations ksu_pkg_iops;

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
		track_throne(false);
	return ret;
}

static void ksu_pkg_observer_init(void)
{
	struct path path;
	struct inode *dir;
	const struct inode_operations *iops;
	int ret;

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
}

#else

/*
 * ! this is on inode_rename, NOT fsnotify
 * we have access to LSM and overhead is way lower.
 * we watch one file, check ifs on the same parent inode.
 * a few int compare and a ptr compare. thats it.
 * as for throne tracker, we just async it by hand
 * by offloading it to a kthread.
 * reuses code from: https://github.com/tiann/KernelSU/blob/v1.0.5/kernel/core_hook.c#L188
 */

static void *system_dir_inode_ptr = nullptr;

static noinline void ksu_grab_data_system_inode()
{
	struct path path;
	int ret = kern_path("/data/system", LOOKUP_FOLLOW, &path);
	if (ret) {
		pr_info("renameat: /data/system not ready? ret: (%d)\n", ret);
		return;
	}

	system_dir_inode_ptr = (void *)d_inode(path.dentry);
	pr_info("renameat: cached /data/system d_inode: 0x%lx\n", system_dir_inode_ptr);
	path_put(&path);
}

static void ksu_rename_observer_slow(struct dentry *old_dentry, struct dentry *new_dentry)
{
	system_dir_inode_ptr = nullptr; // reset cached inode

	char path[128];
	char *buf = dentry_path_raw(new_dentry, path, sizeof(path) - 1);
	if (IS_ERR(buf)) {
		pr_err("dentry_path_raw failed.\n");
		return;
	}

	if (!strnstr(buf, "/system/packages.list", 128))
		return;

	pr_info("renameat: %s -> %s, new path: %s\n", old_dentry->d_iname, new_dentry->d_iname, buf);
	track_throne(false);
	return;
}

static inline void ksu_rename_observer(struct dentry *old_dentry, struct dentry *new_dentry)
{
	// skip kernel threads
	if (!current->mm)
		return;

	if (!old_dentry || !new_dentry)
		return;

	// skip non system uid
	if (likely(current_uid().val != 1000))
		return;

	constexpr unsigned char plist[] = "packages.list";

	// HASH_LEN_DECLARE see dcache.h
	if (likely(new_dentry->d_name.len != sizeof(plist) - 1  ))
		return;

	// /data/system/packages.list.tmp -> /data/system/packages.list
	if (likely(!!memcmp_inline(new_dentry->d_iname, plist, sizeof(plist) - 1 )))
		return;

	// cache dir inode, we try to go for fast path, lockless
	if (unlikely(!system_dir_inode_ptr))
		ksu_grab_data_system_inode();

	if (unlikely(!system_dir_inode_ptr))
		goto slow_path;

	if (unlikely(!new_dentry->d_parent || !new_dentry->d_parent->d_inode))
		goto slow_path;

	/*
	 * fallback to slow path, but this should NOT change unless someone overlays /data/system
	 * but then again maybe https://github.com/tiann/KernelSU/pull/2633#discussion_r2141740346
	 * but /data is casefolded, overlaying is really really unlikely
	 * we self heal this thing, so on enxt run, it will try to grab d inode again
	 * alternatively we can use packages.list inode change as trigger too, however,
	 * we need to save last state. more writes.
	 */
	if (unlikely((void *)new_dentry->d_parent->d_inode != system_dir_inode_ptr))
		goto slow_path;

	pr_info("renameat: %s -> %s, /data/system d_inode: 0x%lx \n", old_dentry->d_iname, new_dentry->d_iname, system_dir_inode_ptr);
	track_throne(false);
	return;

slow_path:
	ksu_rename_observer_slow(old_dentry, new_dentry);
	return;
}

#endif
