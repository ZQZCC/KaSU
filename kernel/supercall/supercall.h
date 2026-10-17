#ifndef __KSU_H_SUPERCALL
#define __KSU_H_SUPERCALL

// IOCTL handler types
typedef int (*ksu_ioctl_handler_t)(bool manager, void __user *arg);
typedef bool (*ksu_perm_check_t)(bool manager);

// IOCTL command mapping
struct ksu_ioctl_cmd_map {
	unsigned int cmd;
	const char *name;
	ksu_ioctl_handler_t handler;
	ksu_perm_check_t perm_check; // Permission check function
	bool allow_su_session;
};

int ksu_open_driver(struct file *filp);
int ksu_release_driver(struct inode *inode, struct file *filp);
bool ksu_is_manager_fd(const struct file *filp);

void ksu_supercalls_init(void);
void ksu_supercalls_exit(void);

#endif // __KSU_H_SUPERCALLS
