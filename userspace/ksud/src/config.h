// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KSUD_CONFIG_H
#define KSUD_CONFIG_H

#include "native.h"

struct ksu_config_view {
	struct ksu_string_map merged;
	unsigned char *temporary_data;
};

int ksu_config_view_load(const char *directory, struct ksu_config_view *view);
void ksu_config_view_free(struct ksu_config_view *view);

#endif
