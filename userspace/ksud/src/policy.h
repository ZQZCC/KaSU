// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KSUD_POLICY_H
#define KSUD_POLICY_H

#include <stdbool.h>
#include <stddef.h>

#define KSU_POLICY_PROFILE_SIZE 784
#define KSU_POLICY_MAX_PROFILES 65535

struct ksu_saved_policy {
	unsigned char *profiles;
	size_t length;
	bool needs_rewrite;
};

void ksu_policy_free(struct ksu_saved_policy *policy);
int ksu_policy_decode(const unsigned char *data, size_t length, struct ksu_saved_policy *policy);
int ksu_policy_load(const char *path, struct ksu_saved_policy *policy);
int ksu_policy_save(const char *path, const unsigned char *profiles, size_t length);

#endif
