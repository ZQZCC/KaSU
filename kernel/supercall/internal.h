#ifndef __KSU_H_SUPERCALL_INTERNAL
#define __KSU_H_SUPERCALL_INTERNAL

bool only_manager(bool manager);
bool only_root(bool manager);
bool manager_or_root(bool manager);
bool always_allow(bool manager);

long ksu_supercall_handle_ioctl(bool manager, unsigned int cmd, void __user *argp, bool su_session);
void ksu_supercall_dump_commands(void);
void ksu_supercall_cleanup_state(void);

#endif // __KSU_H_SUPERCALL_INTERNAL
