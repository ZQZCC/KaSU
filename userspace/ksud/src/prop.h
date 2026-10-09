// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KSUD_PROP_H
#define KSUD_PROP_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define KSU_PROP_SKIP_SVC 1u
#define KSU_PROP_PERSISTENT 2u
#define KSU_PROP_PERSIST_ONLY 4u
#define KSU_PROP_VERBOSE 8u
#define KSU_PROP_CONTEXT 16u
#define KSU_PROP_WAIT 32u
#define KSU_PROP_DELETE 64u
#define KSU_PROP_REBUILD 128u
#define KSU_PROP_FORCE 256u

struct ksu_resetprop {
	unsigned flags;
	const unsigned char *name, *value;
	size_t name_length, value_length;
	const char *file;
	const struct timespec *timeout;
};

int ksu_prop_init(void);
bool ksu_property_equals(const char *name, const char *value);
int ksu_prop_set(const unsigned char *name, size_t name_length, const unsigned char *value,
		 size_t value_length, bool skip_service, bool *need_rebuild);
int ksu_prop_wait(const unsigned char *name, size_t name_length, const unsigned char *old_value,
		  size_t old_length, const struct timespec *timeout);
int ksu_prop_load_file(const char *path, unsigned flags, bool *need_rebuild);
int ksu_resetprop_run(const struct ksu_resetprop *options);

#endif
