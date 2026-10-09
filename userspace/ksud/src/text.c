// SPDX-License-Identifier: GPL-3.0-or-later
#include "text.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static uint32_t utf8_next(const unsigned char **cursor, const unsigned char *end, uint32_t invalid)
{
	const unsigned char *p = *cursor;
	unsigned char first = *p++;
	uint32_t value;
	unsigned count, lower = 0x80, upper = 0xbf;
	if (first < 0x80) {
		*cursor = p;
		return first;
	}
	if (first >= 0xc2 && first <= 0xdf) {
		value = first & 0x1f;
		count = 1;
	} else if (first >= 0xe0 && first <= 0xef) {
		value = first & 0xf;
		count = 2;
		if (first == 0xe0)
			lower = 0xa0;
		if (first == 0xed)
			upper = 0x9f;
	} else if (first >= 0xf0 && first <= 0xf4) {
		value = first & 7;
		count = 3;
		if (first == 0xf0)
			lower = 0x90;
		if (first == 0xf4)
			upper = 0x8f;
	} else {
		*cursor = p;
		return invalid;
	}
	while (count--) {
		if (p == end || *p < lower || *p > upper) {
			*cursor = p;
			return invalid;
		}
		value = (value << 6) | (*p++ & 0x3f);
		lower = 0x80;
		upper = 0xbf;
	}
	*cursor = p;
	return value;
}

uint32_t ksu_utf8_next(const unsigned char **cursor, const unsigned char *end)
{
	return utf8_next(cursor, end, 0xfffd);
}

bool ksu_valid_utf8(const void *data, size_t length)
{
	const unsigned char *p = data, *end = p + length;
	while (p < end) {
		if (*p < 0x80)
			++p;
		else if (utf8_next(&p, end, UINT32_MAX) == UINT32_MAX)
			return false;
	}
	return true;
}

size_t ksu_utf8_put(unsigned char *out, uint32_t value)
{
	if (value < 0x80) {
		out[0] = (unsigned char)value;
		return 1;
	}
	if (value < 0x800) {
		out[0] = 0xc0 | (value >> 6);
		out[1] = 0x80 | (value & 0x3f);
		return 2;
	}
	if (value < 0x10000) {
		out[0] = 0xe0 | (value >> 12);
		out[1] = 0x80 | ((value >> 6) & 0x3f);
		out[2] = 0x80 | (value & 0x3f);
		return 3;
	}
	out[0] = 0xf0 | (value >> 18);
	out[1] = 0x80 | ((value >> 12) & 0x3f);
	out[2] = 0x80 | ((value >> 6) & 0x3f);
	out[3] = 0x80 | (value & 0x3f);
	return 4;
}

size_t ksu_utf8_copy(unsigned char *out, const unsigned char *data, size_t length)
{
	const unsigned char *cursor = data, *end = data + length;
	size_t size = 0;
	while (cursor < end)
		size += ksu_utf8_put(out + size, ksu_utf8_next(&cursor, end));
	return size;
}

bool ksu_unicode_space(uint32_t value)
{
	return (value >= 9 && value <= 13) || value == 0x20 || value == 0x85 || value == 0xa0 ||
	       value == 0x1680 || (value >= 0x2000 && value <= 0x200a) || value == 0x2028 ||
	       value == 0x2029 || value == 0x202f || value == 0x205f || value == 0x3000;
}

static uint32_t utf16_unit(const unsigned char *p, bool little_endian)
{
	return little_endian ? p[0] | (p[1] << 8) : (p[0] << 8) | p[1];
}

unsigned char *ksu_decode_text(unsigned char *data, size_t length, size_t *decoded_length)
{
	if (length > (SIZE_MAX - 1) / 3) {
		free(data);
		errno = EOVERFLOW;
		return NULL;
	}
	const unsigned char *p = data, *end = data + length;
	bool utf16 =
	    length >= 2 && ((p[0] == 0xff && p[1] == 0xfe) || (p[0] == 0xfe && p[1] == 0xff));
	bool little_endian = utf16 && p[0] == 0xff;
	if (utf16)
		p += 2;
	else if (length >= 3 && !memcmp(p, "\xef\xbb\xbf", 3))
		p += 3;
	size_t available = (size_t)(end - p);
	if (!utf16 && ksu_valid_utf8(p, available)) {
		if (p != data)
			memmove(data, p, available);
		data[available] = 0;
		*decoded_length = available;
		return data;
	}
	unsigned char *out = malloc(length * 3 + 1);
	if (!out) {
		free(data);
		return NULL;
	}
	size_t used = 0;
	while (p < end) {
		uint32_t value;
		if (!utf16) {
			const unsigned char *start = p;
			value = ksu_utf8_next(&p, end);
			unsigned expected = *start < 0xe0 ? 2 : *start < 0xf0 ? 3 : 4;
			if (value == 0xfffd && p == end && *start >= 0xc2 && *start <= 0xf4 &&
			    (size_t)(p - start) < expected)
				goto incomplete;
		} else if (end - p < 2) {
			goto incomplete;
		} else {
			value = utf16_unit(p, little_endian);
			p += 2;
			if (value >= 0xd800 && value <= 0xdbff) {
				if (end - p < 2)
					goto incomplete;
				uint32_t low = end - p >= 2 ? utf16_unit(p, little_endian) : 0;
				if (low >= 0xdc00 && low <= 0xdfff) {
					value = 0x10000 + ((value - 0xd800) << 10) + low - 0xdc00;
					p += 2;
				} else
					value = 0xfffd;
			} else if (value >= 0xdc00 && value <= 0xdfff)
				value = 0xfffd;
		}
		used += ksu_utf8_put(out + used, value);
	}
	out[used] = 0;
	*decoded_length = used;
	free(data);
	return out;
incomplete:
	free(data);
	free(out);
	errno = EILSEQ;
	return NULL;
}

void ksu_strings_free(struct ksu_string_map *map)
{
	free(map->entries);
	free(map->data);
	*map = (struct ksu_string_map){0};
}

const struct ksu_string_pair *ksu_strings_find(const struct ksu_string_map *map,
					       const unsigned char *key, size_t length)
{
	for (size_t i = 0; i < map->count; ++i)
		if (map->entries[i].key_length == length &&
		    !memcmp(map->entries[i].key, key, length))
			return map->entries + i;
	return NULL;
}

void ksu_strings_put(struct ksu_string_map *map, struct ksu_string_pair pair)
{
	const struct ksu_string_pair *previous = ksu_strings_find(map, pair.key, pair.key_length);
	if (previous)
		map->entries[previous - map->entries] = pair;
	else
		map->entries[map->count++] = pair;
}
int ksu_valid_identifier(const unsigned char *text, size_t length)
{
	if (length < 2 || !((*text >= 'a' && *text <= 'z') || (*text >= 'A' && *text <= 'Z')))
		return false;
	for (size_t i = 1; i < length; ++i)
		if (!((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z') ||
		      (text[i] >= '0' && text[i] <= '9') || text[i] == '.' || text[i] == '_' ||
		      text[i] == '-'))
			return false;
	return true;
}

void ksu_trim_space(const unsigned char **text, size_t *length)
{
	const unsigned char *start = *text, *end = start + *length, *cursor = start;
	while (cursor < end) {
		const unsigned char *character = cursor;
		if (!ksu_unicode_space(ksu_utf8_next(&cursor, end))) {
			start = character;
			break;
		}
		start = cursor;
	}
	const unsigned char *last = start;
	cursor = start;
	while (cursor < end)
		if (!ksu_unicode_space(ksu_utf8_next(&cursor, end)))
			last = cursor;
	*text = start;
	*length = (size_t)(last - start);
}

int ksu_text_true(const unsigned char *text, size_t length)
{
	ksu_trim_space(&text, &length);
	return (length == 1 && *text == '1') ||
	       (length == 4 && (text[0] | 32) == 't' && (text[1] | 32) == 'r' &&
		(text[2] | 32) == 'u' && (text[3] | 32) == 'e');
}
