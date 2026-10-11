#define KSU_DRIVER_PERMISSION_SU_SESSION (1UL << 0)

struct ksu_driver_context {
	unsigned long permissions;
	struct file *manager_dir;
	struct dentry *manager_parent;
};

static void ksu_free_driver_context(struct ksu_driver_context *context)
{
	if (context->manager_dir) {
		dput(context->manager_parent);
		fput(context->manager_dir);
	}
	kfree(context);
}

static int anon_ksu_release(struct inode *inode, struct file *filp)
{
	ksu_free_driver_context(filp->private_data);
	pr_info("ksu fd released\n");
	return 0;
}

static long anon_ksu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return ksu_supercall_handle_ioctl(filp, cmd, (void __user *)arg);
}

// File operations structure
static const struct file_operations anon_ksu_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = anon_ksu_ioctl,
	.compat_ioctl = anon_ksu_ioctl,
	.release = anon_ksu_release,
};

// Install KSU fd to current process
static int ksu_install_fd_with_permissions(unsigned int fd_flags, unsigned long permissions)
{
	struct ksu_driver_context *context;
	struct file *filp;
	const char *name;
	int fd;

	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;

	context->manager_dir = ksu_get_manager_directory(current_uid().val);
	if (IS_ERR(context->manager_dir)) {
		int err = PTR_ERR(context->manager_dir);

		kfree(context);
		return err;
	}
	if (context->manager_dir)
		context->manager_parent = dget_parent(context->manager_dir->f_path.dentry);
	context->permissions = permissions;
	name = permissions & KSU_DRIVER_PERMISSION_SU_SESSION ? "[ksu_driver_su]" : "[ksu_driver]";

	fd = get_unused_fd_flags(fd_flags);
	if (fd < 0) {
		pr_err("ksu_install_fd: failed to get unused fd\n");
		ksu_free_driver_context(context);
		return fd;
	}

	filp = anon_inode_getfile(name, &anon_ksu_fops, context, O_RDWR);
	if (IS_ERR(filp)) {
		pr_err("ksu_install_fd: failed to create anon inode file\n");
		put_unused_fd(fd);
		ksu_free_driver_context(context);
		return PTR_ERR(filp);
	}

	fd_install(fd, filp);
	pr_info("ksu fd installed: %d for pid %d\n", fd, current->pid);
	return fd;
}

int ksu_install_fd(void)
{
	return ksu_install_fd_with_permissions(O_CLOEXEC, 0);
}

int ksu_install_su_fd(void)
{
	// Preserve this descriptor across the launcher exec into ksud.
	return ksu_install_fd_with_permissions(0, KSU_DRIVER_PERMISSION_SU_SESSION);
}

bool ksu_is_su_session_fd(const struct file *filp)
{
	const struct ksu_driver_context *context = filp->private_data;

	return context && (context->permissions & KSU_DRIVER_PERMISSION_SU_SESSION);
}

bool ksu_is_manager_fd(const struct file *filp)
{
	const struct ksu_driver_context *context = filp->private_data;
	struct dentry *dentry;
	struct inode *inode;
	bool valid;

	if (!context->manager_dir || !uid_eq(filp->f_cred->uid, current_uid()))
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
