// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KSUD_BOOT_H
#define KSUD_BOOT_H

#include <stdbool.h>
#include <stdint.h>

bool ksu_uapi_matches(void);
bool ksu_is_safe_mode(void);
int ksu_bootloader_hide_enabled(void);
enum ksu_boot_stage {
	KSU_STAGE_POST_FS_DATA,
	KSU_STAGE_POST_MOUNT,
	KSU_STAGE_SERVICE,
	KSU_STAGE_BOOT_COMPLETED,
	KSU_STAGE_SOFT_REBOOT,
};
/* Positive deadlines are absolute CLOCK_MONOTONIC nanoseconds; 0 does not wait. */
void ksu_run_stage(enum ksu_boot_stage stage, int64_t deadline);
int ksu_regenerate_boot_rc(void);
int ksu_disable_modules(void);
int ksu_post_fs_data(void);
void ksu_services(void);
void ksu_boot_completed(void);
int ksu_soft_reboot(void);

#endif
