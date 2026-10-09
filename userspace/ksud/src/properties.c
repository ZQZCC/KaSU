// SPDX-License-Identifier: GPL-3.0-or-later
#include "file.h"
#include "native.h"
#include "text.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static bool separator_space(unsigned char value)
{
	return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f';
}

static unsigned char *trim_start(unsigned char *p, const unsigned char *end)
{
	while (p < end) {
		const unsigned char *next = p;
		if (!ksu_unicode_space(ksu_utf8_next(&next, end)))
			break;
		p = (unsigned char *)next;
	}
	return p;
}

static int unescape(unsigned char *p, size_t length, size_t *written)
{
	unsigned char *out = p, *start = p, *end = p + length;
	while (p < end) {
		if (*p != '\\') {
			*out++ = *p++;
			continue;
		}
		++p;
		if (p == end) {
			*out++ = 0;
			break;
		}
		unsigned char value = *p++;
		switch (value) {
		case 't':
			*out++ = '\t';
			break;
		case 'n':
			*out++ = '\n';
			break;
		case 'f':
			*out++ = '\f';
			break;
		case 'r':
			*out++ = '\r';
			break;
		case 'u': {
			uint32_t code = 0;
			for (unsigned i = 0; i < 4; ++i) {
				if (p == end)
					goto invalid;
				unsigned char digit = *p++;
				if (!i && digit == '+')
					continue;
				unsigned hex;
				if (digit >= '0' && digit <= '9')
					hex = digit - '0';
				else if (digit >= 'a' && digit <= 'f')
					hex = digit - 'a' + 10;
				else if (digit >= 'A' && digit <= 'F')
					hex = digit - 'A' + 10;
				else
					goto invalid;
				code = (code << 4) | hex;
			}
			if (code >= 0xd800 && code <= 0xdfff)
				goto invalid;
			out += ksu_utf8_put(out, code);
			break;
		}
		default:
			*out++ = value;
			break;
		}
	}
	*written = (size_t)(out - start);
	return 0;
invalid:
	errno = EINVAL;
	return -1;
}

static int parse_line(unsigned char *line, size_t length, struct ksu_string_map *map)
{
	unsigned char *p = line, *end = line + length;
	while (p < end && separator_space(*p))
		++p;
	if (p == end)
		return 0;
	if (*p == '#' || *p == '!') {
		++p;
		while (p < end && separator_space(*p))
			++p;
		while (end > p && separator_space(end[-1]))
			--end;
		size_t ignored;
		return unescape(p, (size_t)(end - p), &ignored);
	}
	unsigned char *key = p;
	while (p < end && *p != ':' && *p != '=' && !separator_space(*p)) {
		if (*p++ == '\\' && p < end)
			++p;
	}
	size_t key_length = (size_t)(p - key);
	while (p < end && separator_space(*p))
		++p;
	if (p < end && (*p == ':' || *p == '='))
		++p;
	while (p < end && separator_space(*p))
		++p;
	struct ksu_string_pair pair = {.key = key, .value = p};
	if (unescape(key, key_length, &pair.key_length) ||
	    unescape(p, (size_t)(end - p), &pair.value_length))
		return -1;
	ksu_strings_put(map, pair);
	return 0;
}

int ksu_properties_parse(unsigned char *data, size_t length, struct ksu_string_map *map)
{
	*map = (struct ksu_string_map){0};
	size_t decoded;
	map->data = ksu_decode_text(data, length, &decoded);
	if (!map->data)
		return -1;
	size_t capacity = 1;
	for (size_t i = 0; i < decoded; ++i)
		if (map->data[i] == '\r' || map->data[i] == '\n')
			++capacity;
	map->entries = calloc(capacity, sizeof(*map->entries));
	if (!map->entries)
		goto error;
	unsigned char *read = map->data, *end = read + decoded;
	while (read < end) {
		unsigned char *logical = read, *write = read;
		bool first = true;
		for (;;) {
			unsigned char *line = read;
			while (read < end && *read != '\r' && *read != '\n')
				++read;
			unsigned char *line_end = read;
			bool eof = read == end;
			if (!eof) {
				if (*read++ == '\r' && read < end && *read == '\n')
					++read;
			}
			unsigned char *part = first ? line : trim_start(line, line_end);
			unsigned char *comment = line;
			while (comment < line_end && separator_space(*comment))
				++comment;
			bool is_comment =
			    first && comment < line_end && (*comment == '#' || *comment == '!');
			size_t slashes = 0;
			for (unsigned char *p = line_end; p > line && p[-1] == '\\'; --p)
				++slashes;
			if (write != part)
				memmove(write, part, (size_t)(line_end - part));
			write += line_end - part;
			if (is_comment || !(slashes & 1))
				break;
			--write;
			/* The old iterator discards an unfinished continuation at EOF. */
			if (eof) {
				write = logical;
				break;
			}
			first = false;
		}
		if (write != logical && parse_line(logical, (size_t)(write - logical), map))
			goto error;
	}
	return 0;
error:
	int error = errno;
	ksu_strings_free(map);
	errno = error;
	return -1;
}

int ksu_properties_load(const char *path, struct ksu_string_map *map)
{
	*map = (struct ksu_string_map){0};
	size_t length;
	unsigned char *data = ksu_read_file(path, &length);
	return data ? ksu_properties_parse(data, length, map) : -1;
}
