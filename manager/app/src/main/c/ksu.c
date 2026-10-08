#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "ksu.h"

static int fd = -1;

static int open_tinyfs_driver_fd(void) {
    int control = open(KSU_TINYFS_CONTROL_PATH, O_RDONLY | O_CLOEXEC);
    if (control < 0) {
        return -1;
    }
    int driver = ioctl(control, KSU_IOCTL_TINYFS_GET_DRIVER_FD, 0);
    close(control);
    return driver;
}

static int scan_driver_fd(void) {
    const char *kName = "[ksu_driver]";
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) {
        return -1;
    }

    int found = -1;
    struct dirent *de;
    char path[64];
    char target[PATH_MAX];

    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }

        char *endptr = NULL;
        long fd_long = strtol(de->d_name, &endptr, 10);
        if (!de->d_name[0] || *endptr != '\0' || fd_long < 0 || fd_long > INT_MAX) {
            continue;
        }

        snprintf(path, sizeof(path), "/proc/self/fd/%s", de->d_name);
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n < 0) {
            continue;
        }
        target[n] = '\0';

        const char *base = strrchr(target, '/');
        base = base ? base + 1 : target;

        if (strstr(base, kName)) {
            found = (int)fd_long;
            break;
        }
    }

    closedir(dir);
    return found;
}

static int ksuctl(unsigned long op, void *arg) {
    if (fd < 0) {
        fd = scan_driver_fd();
        if (fd < 0) {
            fd = open_tinyfs_driver_fd();
        }
    }

    return ioctl(fd, op, arg);
}

static struct ksu_get_info_cmd g_version;

static struct ksu_get_info_cmd get_info(void) {
    if (!g_version.version) {
        if (ksuctl(KSU_IOCTL_GET_INFO, &g_version) < 0) {
            ksuctl(KSU_IOCTL_GET_INFO_LEGACY, &g_version);
            g_version.uapi_version = 0;
        }
    }
    return g_version;
}

uint32_t get_kernel_uapi_version(void) {
    return get_info().uapi_version;
}

uint32_t get_manager_uapi_version(void) {
    return KERNEL_SU_UAPI_VERSION;
}

uint32_t get_version(void) {
    return get_info().version;
}

int legacy_get_version(void) {
    int32_t version = -1;
    int32_t flags = 0;
    int32_t result = 0;
    prctl(0xDEADBEEF, 2, &version, &flags, &result);
    return version;
}

bool get_allow_list(struct ksu_new_get_allow_list_cmd *cmd) {
    return ksuctl(KSU_IOCTL_NEW_GET_ALLOW_LIST, cmd) == 0;
}

bool is_safe_mode(void) {
    struct ksu_check_safemode_cmd cmd = {0};
    ksuctl(KSU_IOCTL_CHECK_SAFEMODE, &cmd);
    return cmd.in_safe_mode;
}

bool is_manager(void) {
    return true;
}

bool is_pr_build(void) {
    struct ksu_get_info_cmd info = get_info();
    if (info.version > 0) {
        return (info.flags & KSU_GET_INFO_FLAG_PR_BUILD) != 0;
    }
    return false;
}

bool uid_should_umount(int uid) {
    struct ksu_uid_should_umount_cmd cmd = {0};
    cmd.uid = uid;
    ksuctl(KSU_IOCTL_UID_SHOULD_UMOUNT, &cmd);
    return cmd.should_umount;
}

bool set_app_profile(const struct app_profile *profile) {
    struct ksu_set_app_profile_cmd cmd = {0};
    cmd.profile = *profile;
    return ksuctl(KSU_IOCTL_SET_APP_PROFILE, &cmd) == 0;
}

int get_app_profile(struct app_profile *profile) {
    struct ksu_get_app_profile_cmd cmd = {.profile = *profile};
    int ret = ksuctl(KSU_IOCTL_GET_APP_PROFILE, &cmd);
    *profile = cmd.profile;
    return ret;
}

bool set_su_enabled(bool enabled) {
    struct ksu_set_feature_cmd cmd = {0};
    cmd.feature_id = KSU_FEATURE_SU_COMPAT;
    cmd.value = enabled ? 1 : 0;
    return ksuctl(KSU_IOCTL_SET_FEATURE, &cmd) == 0;
}

bool is_su_enabled(void) {
    struct ksu_get_feature_cmd cmd = {0};
    cmd.feature_id = KSU_FEATURE_SU_COMPAT;
    if (ksuctl(KSU_IOCTL_GET_FEATURE, &cmd) != 0) {
        return false;
    }
    if (!cmd.supported) {
        return false;
    }
    return cmd.value != 0;
}

static inline bool get_feature(uint32_t feature_id, uint64_t *out_value, bool *out_supported) {
    struct ksu_get_feature_cmd cmd = {0};
    cmd.feature_id = feature_id;
    if (ksuctl(KSU_IOCTL_GET_FEATURE, &cmd) != 0) {
        return false;
    }
    if (out_value) *out_value = cmd.value;
    if (out_supported) *out_supported = cmd.supported;
    return true;
}

static inline bool set_feature(uint32_t feature_id, uint64_t value) {
    struct ksu_set_feature_cmd cmd = {0};
    cmd.feature_id = feature_id;
    cmd.value = value;
    return ksuctl(KSU_IOCTL_SET_FEATURE, &cmd) == 0;
}

bool set_kernel_umount_enabled(bool enabled) {
    return set_feature(KSU_FEATURE_KERNEL_UMOUNT, enabled ? 1 : 0);
}

bool is_kernel_umount_enabled(void) {
    uint64_t value = 0;
    bool supported = false;
    if (!get_feature(KSU_FEATURE_KERNEL_UMOUNT, &value, &supported)) {
        return false;
    }
    if (!supported) {
        return false;
    }
    return value != 0;
}

bool is_kernel_umount_supported(void) {
    bool supported = false;
    return get_feature(KSU_FEATURE_KERNEL_UMOUNT, NULL, &supported) && supported;
}

int set_selinux_hide_enabled(bool enabled) {
    if (!set_feature(KSU_FEATURE_SELINUX_HIDE, enabled ? 1 : 0)) {
        return -errno;
    }
    return 0;
}

bool is_selinux_hide_enabled(void) {
    uint64_t value = 0;
    bool supported = false;
    if (!get_feature(KSU_FEATURE_SELINUX_HIDE, &value, &supported)) {
        return false;
    }
    if (!supported) {
        return false;
    }
    return value != 0;
}
