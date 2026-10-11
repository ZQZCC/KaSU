#ifndef __KSU_H_SUPERCALL_INTERNAL
#define __KSU_H_SUPERCALL_INTERNAL

bool only_manager(const struct file *filp);
bool only_root(const struct file *filp);
bool manager_or_root(const struct file *filp);
bool always_allow(const struct file *filp);

long ksu_supercall_handle_ioctl(const struct file *filp, unsigned int cmd, void __user *argp);
void ksu_supercall_dump_commands(void);
void ksu_supercall_cleanup_state(void);

#endif // __KSU_H_SUPERCALL_INTERNAL
