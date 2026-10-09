// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KSUD_CLI_H
#define KSUD_CLI_H

#include <stdbool.h>
#include <stdint.h>

struct option;
bool ksu_option_known(const char *arg, const char *shorts, const struct option *options);

int ksu_cli_main(int argc, char **argv);
int ksu_resetprop_main(int argc, char **argv);
int ksu_su_main(int argc, char **argv);
int ksu_debug_su(int global_mnt);
int ksu_debug_manager(const char *package);
int ksu_debug_mark(unsigned operation, int32_t pid);
int ksu_apk_signature(const char *path, uint32_t *size, char hash[65]);

#endif
