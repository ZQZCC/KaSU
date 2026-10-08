#ifndef KERNELSU_KSU_H
#define KERNELSU_KSU_H

#include <stdbool.h>
#include <stdint.h>

#include "uapi/ksu.h"

uint32_t get_kernel_uapi_version(void);

uint32_t get_manager_uapi_version(void);

uint32_t get_version(void);
int legacy_get_version(void);

bool uid_should_umount(int uid);

bool is_safe_mode(void);
bool is_manager(void);

bool is_pr_build(void);

typedef char p_key_t[KSU_MAX_PACKAGE_NAME];

bool set_app_profile(const struct app_profile *profile);

int get_app_profile(struct app_profile *profile);

// Su compat
bool set_su_enabled(bool enabled);

bool is_su_enabled(void);

// Kernel umount
bool set_kernel_umount_enabled(bool enabled);

bool is_kernel_umount_enabled(void);
bool is_kernel_umount_supported(void);

// SELinux hide
int set_selinux_hide_enabled(bool enabled);

bool is_selinux_hide_enabled(void);

bool get_allow_list(struct ksu_new_get_allow_list_cmd *);

#endif //KERNELSU_KSU_H
