// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KSUD_PROP_PERSIST_H
#define KSUD_PROP_PERSIST_H

#include "text.h"

int ksu_prop_persist_load(const char *directory, struct ksu_string_map *map);
int ksu_prop_persist_get(const char *directory, const unsigned char *key, size_t key_length,
			 unsigned char **value, size_t *value_length);
int ksu_prop_persist_set(const char *directory, const unsigned char *key, size_t key_length,
			 const unsigned char *value, size_t value_length);
int ksu_prop_persist_delete(const char *directory, const unsigned char *key, size_t key_length);

#endif
