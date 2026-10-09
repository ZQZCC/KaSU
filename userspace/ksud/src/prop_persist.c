// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "prop_persist.h"
#include "file.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

struct proto_cursor {
	const unsigned char *next, *end;
};

static int invalid(void)
{
	errno = EINVAL;
	return -1;
}

static int varint(struct proto_cursor *cursor, uint64_t *value)
{
	*value = 0;
	for (unsigned shift = 0; shift < 70 && cursor->next < cursor->end; shift += 7) {
		unsigned byte = *cursor->next++;
		if (shift == 63 && byte > 1)
			return invalid();
		*value |= (uint64_t)(byte & 127) << shift;
		if (!(byte & 128))
			return 0;
	}
	return invalid();
}

static int proto_key(struct proto_cursor *cursor, uint32_t *key)
{
	uint64_t value;
	if (varint(cursor, &value) || value > UINT32_MAX || !(value >> 3) || (value & 7) > 5)
		return invalid();
	*key = (uint32_t)value;
	return 0;
}

static int proto_blob(struct proto_cursor *cursor, struct proto_cursor *blob)
{
	uint64_t length;
	if (varint(cursor, &length) || length > (uint64_t)(cursor->end - cursor->next))
		return invalid();
	*blob = (struct proto_cursor){cursor->next, cursor->next + length};
	cursor->next = blob->end;
	return 0;
}

static int proto_skip(struct proto_cursor *cursor, uint32_t key, unsigned depth)
{
	if (!depth)
		return invalid();
	uint64_t length;
	switch (key & 7) {
	case 0:
		return varint(cursor, &length);
	case 1:
		length = 8;
		break;
	case 2:
		if (varint(cursor, &length))
			return -1;
		break;
	case 3:
		for (;;) {
			uint32_t nested;
			if (proto_key(cursor, &nested))
				return -1;
			if ((nested & 7) == 4)
				return (nested >> 3) == (key >> 3) ? 0 : invalid();
			if (proto_skip(cursor, nested, depth - 1))
				return -1;
		}
	case 5:
		length = 4;
		break;
	default:
		return invalid();
	}
	if (length > (uint64_t)(cursor->end - cursor->next))
		return invalid();
	cursor->next += length;
	return 0;
}

static int append(struct ksu_string_map *map, size_t *capacity, struct ksu_string_pair pair)
{
	if (map->count == *capacity) {
		size_t next = *capacity ? *capacity * 2 : 8;
		if (next < *capacity || next > SIZE_MAX / sizeof(*map->entries)) {
			errno = EOVERFLOW;
			return -1;
		}
		void *entries = realloc(map->entries, next * sizeof(*map->entries));
		if (!entries)
			return -1;
		map->entries = entries;
		*capacity = next;
	}
	map->entries[map->count++] = pair;
	return 0;
}

static int compare(const void *a, const void *b)
{
	const struct ksu_string_pair *left = a, *right = b;
	size_t length = left->key_length < right->key_length ? left->key_length : right->key_length;
	int result = memcmp(left->key, right->key, length);
	if (!result)
		result =
		    (left->key_length > right->key_length) - (left->key_length < right->key_length);
	return result;
}

static int compare_input_order(const void *a, const void *b)
{
	const struct ksu_string_pair *left = a, *right = b;
	int result = compare(a, b);
	/* Equal names retain their original order within the owned input buffer. */
	return result ? result : (left->key > right->key) - (left->key < right->key);
}

static int parse_proto(unsigned char *data, size_t length, struct ksu_string_map *map)
{
	*map = (struct ksu_string_map){.data = data};
	struct proto_cursor cursor = {data, data + length};
	size_t capacity = 0;
	while (cursor.next < cursor.end) {
		uint32_t key;
		if (proto_key(&cursor, &key))
			goto error;
		if (key >> 3 != 1) {
			if (proto_skip(&cursor, key, 100))
				goto error;
			continue;
		}
		struct proto_cursor record;
		if ((key & 7) != 2 || proto_blob(&cursor, &record)) {
			invalid();
			goto error;
		}
		struct ksu_string_pair pair = {0};
		while (record.next < record.end) {
			if (proto_key(&record, &key))
				goto error;
			unsigned tag = key >> 3;
			if (tag != 1 && tag != 2) {
				if (proto_skip(&record, key, 99))
					goto error;
				continue;
			}
			struct proto_cursor string;
			if ((key & 7) != 2 || proto_blob(&record, &string)) {
				invalid();
				goto error;
			}
			size_t size = (size_t)(string.end - string.next);
			if (!ksu_valid_utf8(string.next, size)) {
				errno = EILSEQ;
				goto error;
			}
			if (tag == 1) {
				pair.key = string.next;
				pair.key_length = size;
			} else {
				pair.value = string.next;
				pair.value_length = size;
			}
		}
		if (pair.key && pair.value && append(map, &capacity, pair))
			goto error;
	}
	if (map->count > 1)
		qsort(map->entries, map->count, sizeof(*map->entries), compare_input_order);
	size_t used = 0;
	for (size_t i = 0; i < map->count; ++i) {
		if (used && !compare(map->entries + used - 1, map->entries + i))
			map->entries[used - 1] = map->entries[i];
		else
			map->entries[used++] = map->entries[i];
	}
	map->count = used;
	return 0;
error:
	int saved = errno;
	ksu_strings_free(map);
	errno = saved;
	return -1;
}

static size_t varint_size(size_t value)
{
	size_t length = 1;
	while (value >= 128) {
		value >>= 7;
		++length;
	}
	return length;
}

static void put_varint(FILE *output, size_t value)
{
	while (value >= 128) {
		fputc((int)(value & 127) | 128, output);
		value >>= 7;
	}
	fputc((int)value, output);
}

static int encode_proto(FILE *output, const struct ksu_string_pair *entries, size_t count)
{
	for (size_t i = 0; i < count; ++i) {
		const struct ksu_string_pair *pair = entries + i;
		size_t size = 2 + varint_size(pair->key_length) + pair->key_length +
			      varint_size(pair->value_length) + pair->value_length;
		fputc(10, output);
		put_varint(output, size);
		fputc(10, output);
		put_varint(output, pair->key_length);
		if (pair->key_length)
			fwrite(pair->key, 1, pair->key_length, output);
		fputc(18, output);
		put_varint(output, pair->value_length);
		if (pair->value_length)
			fwrite(pair->value, 1, pair->value_length, output);
	}
	return ferror(output) ? -1 : 0;
}

static int load_proto(const char *path, struct ksu_string_map *map)
{
	size_t length;
	unsigned char *data = ksu_read_file(path, &length);
	if (!data) {
		*map = (struct ksu_string_map){0};
		return errno == ENOENT ? 0 : -1;
	}
	return parse_proto(data, length, map);
}

static unsigned char *lossy_name(const unsigned char *name, size_t *length)
{
	size_t size = strlen((const char *)name);
	unsigned char *text = malloc(size * 3 + 1);
	if (!text)
		return NULL;
	*length = ksu_utf8_copy(text, name, size);
	return text;
}

static int load_legacy(const char *directory, struct ksu_string_map *map)
{
	*map = (struct ksu_string_map){0};
	DIR *dir = opendir(directory);
	if (!dir)
		return errno == ENOENT ? 0 : -1;
	struct ksu_string_map records = {0};
	size_t capacity = 0, size = 0;
	int result = -1;
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(dir);
		if (!entry) {
			if (errno)
				goto out;
			break;
		}
		if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "persistent_properties"))
			continue;
		char *path = ksu_join_path(directory, entry->d_name);
		if (!path)
			goto out;
		size_t length;
		unsigned char *value = ksu_read_file(path, &length);
		free(path);
		if (!value)
			continue;
		if (!ksu_valid_utf8(value, length)) {
			free(value);
			continue;
		}
		struct ksu_string_pair pair = {.value = value, .value_length = length};
		pair.key = lossy_name((const unsigned char *)entry->d_name, &pair.key_length);
		if (!pair.key || append(&records, &capacity, pair)) {
			free((void *)pair.key);
			free(value);
			goto out;
		}
		size += pair.key_length + length;
	}
	map->data = malloc(size ? size : 1);
	if (!map->data)
		goto out;
	unsigned char *next = map->data;
	for (size_t i = 0; i < records.count; ++i) {
		struct ksu_string_pair *pair = records.entries + i;
		if (pair->key_length)
			memcpy(next, pair->key, pair->key_length);
		free((void *)pair->key);
		pair->key = next;
		next += pair->key_length;
		if (pair->value_length)
			memcpy(next, pair->value, pair->value_length);
		free((void *)pair->value);
		pair->value = next;
		next += pair->value_length;
	}
	map->entries = records.entries;
	map->count = records.count;
	if (map->count > 1)
		qsort(map->entries, map->count, sizeof(*map->entries), compare_input_order);
	result = 0;
out:
	int saved = errno;
	if (result) {
		for (size_t i = 0; i < records.count; ++i) {
			free((void *)records.entries[i].key);
			free((void *)records.entries[i].value);
		}
		free(records.entries);
	}
	closedir(dir);
	errno = saved;
	return result;
}

int ksu_prop_persist_load(const char *directory, struct ksu_string_map *map)
{
	char *path = ksu_join_path(directory, "persistent_properties");
	if (!path)
		return -1;
	int result = access(path, F_OK) ? load_legacy(directory, map) : load_proto(path, map);
	int saved = errno;
	free(path);
	errno = saved;
	return result;
}

static char *legacy_path(const char *directory, const unsigned char *key, size_t length)
{
	if (!length || memchr(key, 0, length) || memchr(key, '/', length) ||
	    memchr(key, '\\', length) || (length == 2 && !memcmp(key, "..", 2))) {
		invalid();
		return NULL;
	}
	char *name = strndup((const char *)key, length);
	if (!name)
		return NULL;
	char *path = ksu_join_path(directory, name);
	free(name);
	return path;
}

int ksu_prop_persist_get(const char *directory, const unsigned char *key, size_t key_length,
			 unsigned char **value, size_t *value_length)
{
	*value = NULL;
	*value_length = 0;
	char *path = ksu_join_path(directory, "persistent_properties");
	if (!path)
		return -1;
	int result = 0;
	if (access(path, F_OK)) {
		free(path);
		path = legacy_path(directory, key, key_length);
		if (!path)
			return -1;
		*value = ksu_read_file(path, value_length);
		if (!*value)
			result = errno == ENOENT ? 0 : -1;
		else if (!ksu_valid_utf8(*value, *value_length)) {
			free(*value);
			*value = NULL;
			*value_length = 0;
			errno = EILSEQ;
			result = -1;
		}
	} else {
		struct ksu_string_map map;
		result = load_proto(path, &map);
		if (!result) {
			const struct ksu_string_pair *pair =
			    ksu_strings_find(&map, key, key_length);
			if (pair) {
				*value = malloc(pair->value_length ? pair->value_length : 1);
				if (!*value)
					result = -1;
				else {
					*value_length = pair->value_length;
					if (*value_length)
						memcpy(*value, pair->value, *value_length);
				}
			}
			ksu_strings_free(&map);
		}
	}
	int saved = errno;
	free(path);
	errno = saved;
	return result;
}

static int write_atomic(const char *path, const struct ksu_string_pair *entries, size_t count,
			bool legacy)
{
	const char *name = strrchr(path, '/');
	name = name ? name + 1 : path;
	if (!*name || !strcmp(name, ".") || !strcmp(name, ".."))
		return invalid();
	char *parent = name == path ? strdup(".") : strndup(path, (size_t)(name - path));
	if (!parent)
		return -1;
	if (ksu_mkdirs(parent)) {
		int saved = errno;
		free(parent);
		errno = saved;
		return -1;
	}
	char *temporary = NULL;
	int fd = -1;
	for (unsigned attempt = 0; attempt < 64; ++attempt) {
		struct timespec now = {0};
		clock_gettime(CLOCK_REALTIME, &now);
		unsigned long long timestamp =
		    (unsigned long long)now.tv_sec * 1000000000 + (unsigned long long)now.tv_nsec;
		free(temporary);
		temporary = NULL;
		if (asprintf(&temporary, "%s/.%s.%d.%llu.%u.tmp", parent, name, getpid(), timestamp,
			     attempt) < 0)
			break;
		fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
		if (fd >= 0 || errno != EEXIST)
			break;
	}
	int result = -1, saved = errno;
	if (fd >= 0) {
		FILE *file = fdopen(fd, "w");
		if (!file) {
			saved = errno;
			close(fd);
		} else {
			bool written = legacy
					   ? (!entries[0].value_length ||
					      fwrite(entries[0].value, 1, entries[0].value_length,
						     file) == entries[0].value_length)
					   : !encode_proto(file, entries, count);
			written = written && !fflush(file) && !fsync(fd);
			saved = errno;
			fclose(file);
			if (written) {
				result = rename(temporary, path);
				saved = errno;
				if (result && (saved == EEXIST || saved == EACCES) &&
				    !access(path, F_OK)) {
					result = unlink(path) ? -1 : rename(temporary, path);
					saved = errno;
				}
			}
		}
		if (result)
			unlink(temporary);
	}
	free(temporary);
	free(parent);
	errno = saved;
	return result;
}

static int update(const char *directory, const unsigned char *key, size_t key_length,
		  const unsigned char *value, size_t value_length)
{
	if (!ksu_valid_utf8(key, key_length) || (value && !ksu_valid_utf8(value, value_length))) {
		errno = EILSEQ;
		return -1;
	}
	struct ksu_string_pair pair = {key, key_length, value, value_length};
	char *path = ksu_join_path(directory, "persistent_properties");
	if (!path)
		return -1;
	int result = -1;
	if (access(path, F_OK)) {
		free(path);
		path = legacy_path(directory, key, key_length);
		if (!path)
			return -1;
		if (value)
			result = write_atomic(path, &pair, 1, true);
		else {
			result = unlink(path);
			if (result && errno == ENOENT)
				result = 0;
		}
	} else {
		unsigned char label[256];
		ssize_t label_length = lgetxattr(path, "security.selinux", label, sizeof(label));
		struct ksu_string_map map;
		if (load_proto(path, &map))
			goto out;
		const struct ksu_string_pair *found = ksu_strings_find(&map, key, key_length);
		size_t index = found ? (size_t)(found - map.entries) : map.count;
		if (value) {
			if (found)
				map.entries[index] = pair;
			else {
				size_t capacity = map.count;
				if (append(&map, &capacity, pair)) {
					ksu_strings_free(&map);
					goto out;
				}
			}
			if (map.count > 1)
				qsort(map.entries, map.count, sizeof(*map.entries), compare);
		} else if (found) {
			--map.count;
			memmove(map.entries + index, map.entries + index + 1,
				(map.count - index) * sizeof(*map.entries));
		}
		result = write_atomic(path, map.entries, map.count, false);
		if (!result && label_length > 0)
			lsetxattr(path, "security.selinux", label, (size_t)label_length, 0);
		ksu_strings_free(&map);
	}
out:
	int saved = errno;
	free(path);
	errno = saved;
	return result;
}

int ksu_prop_persist_set(const char *directory, const unsigned char *key, size_t key_length,
			 const unsigned char *value, size_t value_length)
{
	return update(directory, key, key_length, value, value_length);
}

int ksu_prop_persist_delete(const char *directory, const unsigned char *key, size_t key_length)
{
	return update(directory, key, key_length, NULL, 0);
}
