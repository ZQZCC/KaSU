// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KSUD_PROP_AREA_H
#define KSUD_PROP_AREA_H

#include "text.h"

#define KSU_PROP_AREA_HEADER 128
#define KSU_PROP_VALUE_MAX 92
#define KSU_PROP_LONG_FLAG (1U << 16)

struct ksu_prop_area {
	unsigned char *base;
	size_t length;
	uint32_t capacity;
};

struct ksu_prop_record {
	struct ksu_string_pair pair;
	unsigned char *data;
	uint32_t counter;
};

int ksu_prop_area_init(struct ksu_prop_area *area, void *memory, size_t length);
int ksu_prop_area_empty(struct ksu_prop_area *area, void *memory, size_t length, bool dirty_backup);
int ksu_prop_area_find(const struct ksu_prop_area *area, const unsigned char *name, size_t length,
		       uint32_t *offset);
int ksu_prop_area_emplace(struct ksu_prop_area *area, const unsigned char *name, size_t name_length,
			  const unsigned char *value, size_t value_length, uint32_t counter);
int ksu_prop_area_set(struct ksu_prop_area *area, struct ksu_prop_area *serial_area,
		      const unsigned char *name, size_t name_length, const unsigned char *value,
		      size_t value_length, bool *need_rebuild);
int ksu_prop_area_remove(struct ksu_prop_area *area, const unsigned char *name, size_t length);
int ksu_prop_area_read(const struct ksu_prop_area *area, uint32_t offset,
		       struct ksu_prop_record *record);
int ksu_prop_area_rebuild(struct ksu_prop_area *area, bool check);
void ksu_prop_area_bump(struct ksu_prop_area *area);

#endif
