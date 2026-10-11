struct ksu_driver_context {
	struct file *manager_dir;
	struct dentry *manager_parent;
};

int ksu_open_driver(struct file *filp)
{
	struct file *directory = ksu_get_manager_directory(current_uid().val);
	struct ksu_driver_context *context;

	if (IS_ERR(directory))
		return PTR_ERR(directory);
	if (!directory)
		return 0;

	context = kmalloc(sizeof(*context), GFP_KERNEL);
	if (!context) {
		fput(directory);
		return -ENOMEM;
	}
	context->manager_dir = directory;
	context->manager_parent = dget_parent(directory->f_path.dentry);
	filp->private_data = context;
	return 0;
}

int ksu_release_driver(struct inode *inode, struct file *filp)
{
	struct ksu_driver_context *context = filp->private_data;

	if (context) {
		dput(context->manager_parent);
		fput(context->manager_dir);
		kfree(context);
	}
	pr_info("ksu fd released\n");
	return 0;
}

bool ksu_is_manager_fd(const struct file *filp)
{
	const struct ksu_driver_context *context = filp->private_data;
	struct dentry *dentry;
	struct inode *inode;
	bool valid;

	if (!context || !uid_eq(filp->f_cred->uid, current_uid()))
		return false;

	dentry = context->manager_dir->f_path.dentry;
	inode = file_inode(context->manager_dir);
	/* Uninstall renames or unlinks this object; a replacement is a different inode. */
	spin_lock(&dentry->d_lock);
	valid = !d_unhashed(dentry) && READ_ONCE(inode->i_nlink) &&
		dentry->d_parent == context->manager_parent &&
		dentry->d_name.len == sizeof(KSU_PACKAGE_NAME) - 1 &&
		!memcmp(dentry->d_name.name, KSU_PACKAGE_NAME, sizeof(KSU_PACKAGE_NAME) - 1) &&
		uid_eq(READ_ONCE(inode->i_uid), current_uid());
	spin_unlock(&dentry->d_lock);
	return valid;
}

void __init ksu_supercalls_init(void)
{
	ksu_supercall_dump_commands();
}

void __exit ksu_supercalls_exit(void) { }
