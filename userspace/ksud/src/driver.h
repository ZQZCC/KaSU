// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KSUD_DRIVER_H
#define KSUD_DRIVER_H

#include <stdint.h>

void ksu_setup_sigsys(void);
int ksu_claim_driver_fd(void);
int ksu_driver_fd(void);
int ksu_ioctl(uint32_t request, void *arg);
struct ksu_get_info_cmd;
const struct ksu_get_info_cmd *ksu_get_driver_info(void);

#endif
