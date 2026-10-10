// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KSUD_PATHS_H
#define KSUD_PATHS_H

#ifndef KSU_ADB_DIR
#define KSU_ADB_DIR "/data/adb"
#endif
#ifndef KSU_METADATA_DIR
#define KSU_METADATA_DIR "/metadata"
#endif

#define KSU_WORKING_DIR KSU_ADB_DIR "/ksu"
#define KSU_BOOTLOADER_HIDE_DISABLED KSU_WORKING_DIR "/.disable_bootloader_hide"
#define KSU_BINARY_DIR KSU_WORKING_DIR "/bin"
#define KSU_BUSYBOX_PATH KSU_BINARY_DIR "/busybox"
#define KSU_LOG_DIR KSU_WORKING_DIR "/log"
#define KSU_PROFILE_SELINUX_DIR KSU_WORKING_DIR "/profile/selinux"
#define KSU_PROFILE_TEMPLATE_DIR KSU_WORKING_DIR "/profile/templates"
#define KSU_DAEMON_PATH KSU_ADB_DIR "/ksud"
#define KSU_MODULE_DIR KSU_ADB_DIR "/modules"
#define KSU_MODULE_UPDATE_DIR KSU_ADB_DIR "/modules_update"
#define KSU_METAMODULE_DIR KSU_ADB_DIR "/metamodule/"
#define KSU_MODULE_CONFIG_DIR KSU_WORKING_DIR "/module_configs"
#define KSU_PREINIT_WATCHDOG KSU_METADATA_DIR "/watchdog/ksu"
#define KSU_PREINIT_DEFAULT KSU_METADATA_DIR "/ksu"
#define KSU_POLICY_PATH KSU_WORKING_DIR "/.allowlist"

#endif
