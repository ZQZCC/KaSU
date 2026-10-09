// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KSUD_PROCESS_H
#define KSUD_PROCESS_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

int ksu_switch_mnt_ns(int pid);
int64_t ksu_now_ns(void);
void ksu_switch_cgroups(void);
void ksu_detach_process_group(int use_init_pgrp);
pid_t ksu_spawn(char *const arguments[], char *const environment[], int output, bool detached);
int ksu_wait_child(pid_t pid, int *status);
int ksu_create_daemon(bool init_namespace);

#endif
