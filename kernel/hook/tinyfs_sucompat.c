// SPDX-License-Identifier: GPL-2.0
/*
 * Fixed-kernel TinyFS sucompat.
 *
 * Synthetic files expose a launcher and permission-checked fd bootstrap.
 * Root is granted by the normal KSU control fd, never from a VFS read.
 */
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/pagemap.h>

#include "tinysu_arm64.h"

static struct inode *ksu_tinyfs_su_inode;
static struct inode *ksu_tinyfs_control_inode;
static const struct inode_operations *ksu_tinyfs_orig_bin_iops;
static struct inode_operations ksu_tinyfs_bin_iops;
static struct path ksu_tinyfs_bin_path;
static DEFINE_MUTEX(ksu_tinyfs_init_mutex);
static bool ksu_tinyfs_control_ready;
static bool ksu_tinyfs_ready;

bool ksu_tinyfs_sucompat_ready(void)
{
	return smp_load_acquire(&ksu_tinyfs_ready);
}

static __always_inline bool ksu_tinyfs_su_visible(void)
{
	return READ_ONCE(ksu_su_compat_enabled) &&
	       ksu_is_allow_uid_for_current(current_uid().val);
}

static int ksu_tinyfs_d_revalidate(struct dentry *dentry, unsigned int flags)
{
	struct inode *inode = READ_ONCE(dentry->d_inode);

	if (!inode)
		return 0;
	if (inode == ksu_tinyfs_control_inode)
		return manager_or_root() ? 1 : -ENOENT;

	if (inode != ksu_tinyfs_su_inode)
		return 0;

	/* Keep the SELinux SID-to-context fallback out of RCU-walk. */
	if ((flags & LOOKUP_RCU) && unlikely(current_uid().val == 0))
		return -ECHILD;

	return ksu_tinyfs_su_visible() ? 1 : -ENOENT;
}

static struct dentry_operations ksu_tinyfs_dops;

static void ksu_tinyfs_prepare_dentry(struct dentry *dentry)
{
	spin_lock(&dentry->d_lock);
	dentry->d_op = &ksu_tinyfs_dops;
	dentry->d_flags |= DCACHE_OP_REVALIDATE;
	spin_unlock(&dentry->d_lock);
}

static struct dentry *ksu_tinyfs_lookup(struct inode *dir,
					       struct dentry *dentry,
					       unsigned int flags)
{
	struct inode *inode;
	bool visible;

	if (dentry->d_name.len == 2 &&
	    memcmp(dentry->d_name.name, "su", 2) == 0) {
		if (!ksu_tinyfs_sucompat_ready())
			goto original_lookup;
		inode = ksu_tinyfs_su_inode;
		visible = ksu_tinyfs_su_visible();
	} else if (dentry->d_name.len == 4 &&
		   memcmp(dentry->d_name.name, ".ksu", 4) == 0) {
		if (!smp_load_acquire(&ksu_tinyfs_control_ready))
			goto original_lookup;
		inode = ksu_tinyfs_control_inode;
		visible = manager_or_root();
	} else {
		goto original_lookup;
	}

	ksu_tinyfs_prepare_dentry(dentry);
	d_add(dentry, igrab(inode));
	/* A fresh lookup does not revalidate the dentry it just instantiated. */
	return visible ? NULL : ERR_PTR(-ENOENT);

original_lookup:
	return ksu_tinyfs_orig_bin_iops->lookup(dir, dentry, flags);
}

static long ksu_tinyfs_ioctl(struct file *file, unsigned int cmd,
			   unsigned long arg)
{
	/* Recheck the caller even if this file was opened or passed by another UID. */
	switch (cmd) {
	case KSU_IOCTL_TINYFS_GET_SU_FD:
		if (file_inode(file) != ksu_tinyfs_su_inode ||
		    !ksu_tinyfs_su_visible())
			return -EPERM;
		return ksu_install_su_fd();
	case KSU_IOCTL_TINYFS_GET_DRIVER_FD:
		if (file_inode(file) != ksu_tinyfs_control_inode ||
		    !manager_or_root())
			return -EPERM;
		return ksu_install_fd();
	default:
		return -ENOTTY;
	}
}

static int ksu_tinyfs_read_folio(struct file *file, struct folio *folio)
{
	void *page_addr = kmap_local_folio(folio, 0);
	loff_t offset = folio_pos(folio);
	size_t size = folio_size(folio);

	folio_zero_range(folio, 0, size);
	if (offset < sizeof(ksu_tinysu_arm64)) {
		size_t count = min_t(size_t,
				     sizeof(ksu_tinysu_arm64) - offset, size);
		memcpy(page_addr, ksu_tinysu_arm64 + offset, count);
	}

	kunmap_local(page_addr);
	flush_dcache_folio(folio);
	folio_mark_uptodate(folio);
	folio_unlock(folio);
	return 0;
}

static const struct address_space_operations ksu_tinyfs_aops = {
	.read_folio = ksu_tinyfs_read_folio,
};

static const struct file_operations ksu_tinyfs_fops = {
	.unlocked_ioctl = ksu_tinyfs_ioctl,
	.compat_ioctl = ksu_tinyfs_ioctl,
	.read_iter = generic_file_read_iter,
	.llseek = generic_file_llseek,
	.mmap = generic_file_readonly_mmap,
};

static const struct file_operations ksu_tinyfs_control_fops = {
	.unlocked_ioctl = ksu_tinyfs_ioctl,
	.compat_ioctl = ksu_tinyfs_ioctl,
};

static struct inode *ksu_tinyfs_new_inode(struct super_block *sb,
					const struct file_operations *fops,
					umode_t mode)
{
	struct inode_security_struct *security;
	struct inode *inode = new_inode(sb);

	if (!inode) {
		pr_err("tinyfs: failed to allocate inode\n");
		return NULL;
	}
	inode->i_ino = iunique(sb, 1000000);
	inode->i_mode = S_IFREG | mode;
	inode->i_uid = GLOBAL_ROOT_UID;
	inode->i_gid = GLOBAL_ROOT_GID;
	inode->i_fop = fops;
	inode->i_flags |= S_PRIVATE | S_NOATIME;
	set_nlink(inode, 1);

	security = selinux_inode(inode);
	if (!security || !ksu_file_sid) {
		pr_err("tinyfs: ksu_file SID is unavailable\n");
		iput(inode);
		return NULL;
	}
	spin_lock(&security->lock);
	security->sid = ksu_file_sid;
	security->sclass = SECCLASS_FILE;
	security->initialized = LABEL_INITIALIZED;
	spin_unlock(&security->lock);
	return inode;
}

static void ksu_tinyfs_invalidate_dentry(struct dentry *parent,
					const struct qstr *name)
{
	struct dentry *cached = d_hash_and_lookup(parent, name);

	if (IS_ERR(cached)) {
		pr_warn("tinyfs: failed to invalidate cached %s dentry: %ld\n",
			name->name, PTR_ERR(cached));
	} else if (cached) {
		ksu_tinyfs_prepare_dentry(cached);
		d_invalidate(cached);
		dput(cached);
	}
}

void ksu_tinyfs_control_init(void)
{
	const struct inode_operations *iops;
	struct inode *bin_inode;
	struct inode *inode;
	struct path bin_path;
	struct path control_path;
	struct qstr control_name = QSTR_INIT(".ksu", 4);
	int err;

	if (smp_load_acquire(&ksu_tinyfs_control_ready))
		return;

	mutex_lock(&ksu_tinyfs_init_mutex);
	if (ksu_tinyfs_control_ready)
		goto out_unlock;

	if (kern_path("/system/bin", LOOKUP_FOLLOW, &bin_path)) {
		pr_err("tinyfs: /system/bin is unavailable\n");
		goto out_unlock;
	}

	bin_inode = d_inode(bin_path.dentry);
	iops = READ_ONCE(bin_inode->i_op);
	if (!iops || !iops->lookup) {
		pr_err("tinyfs: /system/bin has no lookup operation\n");
		goto out_path;
	}
	if (bin_inode->i_sb->s_d_op && bin_inode->i_sb->s_d_op->d_revalidate) {
		pr_err("tinyfs: existing d_revalidate is unsupported\n");
		goto out_path;
	}

	err = kern_path(KSU_TINYFS_CONTROL_PATH, 0, &control_path);
	if (!err) {
		path_put(&control_path);
		pr_err("tinyfs: control path already exists\n");
		goto out_path;
	}
	if (err != -ENOENT) {
		pr_err("tinyfs: failed to inspect control path: %d\n", err);
		goto out_path;
	}

	if (bin_inode->i_sb->s_d_op)
		ksu_tinyfs_dops = *bin_inode->i_sb->s_d_op;
	ksu_tinyfs_dops.d_revalidate = ksu_tinyfs_d_revalidate;

	inode = ksu_tinyfs_new_inode(bin_inode->i_sb,
				   &ksu_tinyfs_control_fops, 0444);
	if (!inode)
		goto out_path;

	ksu_tinyfs_orig_bin_iops = iops;
	ksu_tinyfs_control_inode = inode;
	/* Built-in only: keep the directory alive for both initialization stages. */
	ksu_tinyfs_bin_path = bin_path;
	ksu_tinyfs_bin_iops = *iops;
	ksu_tinyfs_bin_iops.lookup = ksu_tinyfs_lookup;
	WRITE_ONCE(bin_inode->i_op, &ksu_tinyfs_bin_iops);
	smp_mb();
	smp_store_release(&ksu_tinyfs_control_ready, true);
	ksu_tinyfs_invalidate_dentry(bin_path.dentry, &control_name);
	pr_info("tinyfs: control endpoint enabled\n");
	goto out_unlock;

out_path:
	path_put(&bin_path);
out_unlock:
	mutex_unlock(&ksu_tinyfs_init_mutex);
}

void ksu_tinyfs_sucompat_init(void)
{
	struct inode *inode;
	struct path ksud_path;
	struct path su_path;
	struct qstr su_name = QSTR_INIT("su", 2);
	int err;

	if (ksu_tinyfs_sucompat_ready())
		return;
	ksu_tinyfs_control_init();
	if (!smp_load_acquire(&ksu_tinyfs_control_ready))
		return;

	mutex_lock(&ksu_tinyfs_init_mutex);
	if (ksu_tinyfs_ready)
		goto out_unlock;

	if (kern_path(KSUD_PATH, LOOKUP_FOLLOW, &ksud_path)) {
		pr_err("tinyfs: %s is unavailable\n", KSUD_PATH);
		goto out_unlock;
	}
	if (!S_ISREG(d_inode(ksud_path.dentry)->i_mode)) {
		pr_err("tinyfs: %s is not a regular file\n", KSUD_PATH);
		path_put(&ksud_path);
		goto out_unlock;
	}
	path_put(&ksud_path);

	err = kern_path("/system/bin/su", 0, &su_path);
	if (!err) {
		path_put(&su_path);
		pr_err("tinyfs: /system/bin/su already exists\n");
		goto out_unlock;
	}
	if (err != -ENOENT) {
		pr_err("tinyfs: failed to inspect /system/bin/su: %d\n", err);
		goto out_unlock;
	}

	inode = ksu_tinyfs_new_inode(d_inode(ksu_tinyfs_bin_path.dentry)->i_sb,
				   &ksu_tinyfs_fops, 0755);
	if (!inode)
		goto out_unlock;
	inode->i_size = sizeof(ksu_tinysu_arm64);
	inode->i_mapping->a_ops = &ksu_tinyfs_aops;
	ksu_tinyfs_su_inode = inode;

	smp_store_release(&ksu_tinyfs_ready, true);
	ksu_sucompat_disable_branch();
	ksu_tinyfs_invalidate_dentry(ksu_tinyfs_bin_path.dentry, &su_name);
	pr_info("tinyfs: synthetic /system/bin/su enabled\n");

out_unlock:
	mutex_unlock(&ksu_tinyfs_init_mutex);
}
