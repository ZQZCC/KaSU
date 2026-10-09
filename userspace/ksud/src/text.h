// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef KSUD_TEXT_H
#define KSUD_TEXT_H

#include "native.h"
#include <stdbool.h>

bool ksu_valid_utf8(const void *data, size_t length);
uint32_t ksu_utf8_next(const unsigned char **cursor, const unsigned char *end);
size_t ksu_utf8_put(unsigned char *out, uint32_t value);
size_t ksu_utf8_copy(unsigned char *out, const unsigned char *data, size_t length);
bool ksu_unicode_space(uint32_t value);
void ksu_trim_space(const unsigned char **text, size_t *length);
/* Consumes a heap buffer with room for a trailing NUL, including on failure. */
unsigned char *ksu_decode_text(unsigned char *data, size_t length, size_t *decoded_length);
void ksu_strings_put(struct ksu_string_map *map, struct ksu_string_pair pair);
const struct ksu_string_pair *ksu_strings_find(const struct ksu_string_map *map,
					       const unsigned char *key, size_t length);

#endif
