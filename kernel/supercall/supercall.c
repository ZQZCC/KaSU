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

// downstream: make sure to pass arg as reference, this can allow us to extend things.
int ksu_handle_sys_reboot(int magic1, int magic2, unsigned int cmd, void __user **arg)
{
	if (magic1 != KSU_INSTALL_MAGIC1)
		return 0;

	// when ternary on fmt?
	// cold syscall, we can splurge xD
	if (magic2 == KSU_INSTALL_MAGIC2)
		pr_info("sys_reboot: magic: 0x%x id: 0x%x pid: %d comm: %s \n", magic1, magic2, current->pid, current->comm);
	else
		pr_info("sys_reboot: magic: 0x%x id: %d pid: %d pid: %s \n", magic1, magic2, current->pid, current->comm);

	// arg4 = (unsigned long)PT_REGS_SYSCALL_PARM4(real_regs);
	// downstream: dereference arg as arg4 so we can be inline to upstream
	void __user *arg4 = (void __user *)*arg;

	if (magic2 == KSU_INSTALL_MAGIC2) {
		int fd = ksu_install_fd();

		pr_info("[%d] install ksu fd: %d\n", current->pid, fd);
		if (copy_to_user((void __user *)arg4, &fd, sizeof(fd))) {
			pr_err("install ksu fd reply err\n");
			ksu_close_fd(fd);
		}

		return 0;
	}

	toolkit_handle_sys_reboot(magic1, magic2, cmd, arg);
	return 0;
}

void __init ksu_supercalls_init(void)
{
	ksu_supercall_dump_commands();
}

void __exit ksu_supercalls_exit(void) { }
