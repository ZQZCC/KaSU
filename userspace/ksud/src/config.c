// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "config.h"
#include "file.h"
#include "native.h"
#include "text.h"

#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONFIG_MAGIC 0x4b53554d
#define CONFIG_VERSION 1

static uint32_t read_u32(const unsigned char *data)
{
	uint32_t value;
	memcpy(&value, data, sizeof(value));
	return le32toh(value);
}

int ksu_config_valid_key(const unsigned char *key, size_t length)
{
	return length <= KSU_CONFIG_MAX_KEY && ksu_valid_identifier(key, length);
}

int ksu_config_load(const char *path, struct ksu_string_map *config)
{
	*config = (struct ksu_string_map){0};
	struct stat st;
	if (stat(path, &st))
		return 0;
	size_t length;
	unsigned char *data = ksu_read_file(path, &length);
	if (!data)
		return -1;
	config->data = data;
	if (length < 12 || read_u32(data) != CONFIG_MAGIC || read_u32(data + 4) != CONFIG_VERSION)
		goto invalid;
	uint32_t count = read_u32(data + 8);
	if (count > (length - 12) / 8)
		goto invalid;
	if (count) {
		config->entries = calloc(count, sizeof(*config->entries));
		if (!config->entries)
			goto error;
	}
	size_t offset = 12;
	for (uint32_t i = 0; i < count; ++i) {
		struct ksu_string_pair entry;
		if (length - offset < 4)
			goto invalid;
		entry.key_length = read_u32(data + offset);
		offset += 4;
		if (entry.key_length > length - offset)
			goto invalid;
		entry.key = data + offset;
		offset += entry.key_length;
		if (length - offset < 4)
			goto invalid;
		entry.value_length = read_u32(data + offset);
		offset += 4;
		if (entry.value_length > length - offset)
			goto invalid;
		entry.value = data + offset;
		offset += entry.value_length;
		if (!ksu_valid_utf8(entry.key, entry.key_length) ||
		    !ksu_valid_utf8(entry.value, entry.value_length)) {
			errno = EILSEQ;
			goto error;
		}
		ksu_strings_put(config, entry);
	}
	return 0;
invalid:
	errno = EINVAL;
error:
	int error = errno;
	ksu_strings_free(config);
	errno = error;
	return -1;
}

static bool write_u32(FILE *file, uint32_t value)
{
	value = htole32(value);
	return fwrite(&value, sizeof(value), 1, file) == 1;
}

static int load_config_file(const char *directory, const char *name, struct ksu_string_map *map)
{
	char *path = ksu_join_path(directory, name);
	if (!path)
		return -1;
	if (ksu_config_load(path, map))
		fprintf(stderr, "Failed to load config %s: %s\n", path, strerror(errno));
	free(path);
	return 0;
}

void ksu_config_view_free(struct ksu_config_view *view)
{
	ksu_strings_free(&view->merged);
	free(view->temporary_data);
	view->temporary_data = NULL;
}

int ksu_config_view_load(const char *directory, struct ksu_config_view *view)
{
	*view = (struct ksu_config_view){0};
	struct ksu_string_map temporary __attribute__((cleanup(ksu_strings_free))) = {0};
	if (load_config_file(directory, "persist.config", &view->merged) ||
	    load_config_file(directory, "tmp.config", &temporary))
		goto error;
	if (!temporary.count)
		return 0;
	if (!view->merged.count) {
		ksu_strings_free(&view->merged);
		view->merged = temporary;
		temporary = (struct ksu_string_map){0};
		return 0;
	}
	size_t count = view->merged.count + temporary.count;
	void *entries = realloc(view->merged.entries, count * sizeof(*view->merged.entries));
	if (!entries)
		goto error;
	view->merged.entries = entries;
	/* Values retain their original buffers; temporary entries replace persistent ones. */
	for (size_t i = 0; i < temporary.count; ++i)
		ksu_strings_put(&view->merged, temporary.entries[i]);
	view->temporary_data = temporary.data;
	temporary.data = NULL;
	return 0;
error:
	ksu_config_view_free(view);
	return -1;
}

int ksu_config_save(const char *directory, const char *path, const struct ksu_string_pair *entries,
		    size_t count)
{
	if (count > KSU_CONFIG_MAX_COUNT) {
		errno = E2BIG;
		return -1;
	}
	for (size_t i = 0; i < count; ++i) {
		if (!ksu_config_valid_key(entries[i].key, entries[i].key_length) ||
		    entries[i].value_length > KSU_CONFIG_MAX_VALUE) {
			errno = EINVAL;
			return -1;
		}
	}
	if (ksu_mkdirs(directory))
		return -1;
	const char *extension = strrchr(path, '.');
	size_t prefix =
	    extension && !strchr(extension, '/') ? (size_t)(extension - path) : strlen(path);
	char *temporary;
	if (asprintf(&temporary, "%.*s.tmp", (int)prefix, path) < 0)
		return -1;
	FILE *file = fopen(temporary, "we");
	if (!file) {
		free(temporary);
		return -1;
	}
	bool written = write_u32(file, CONFIG_MAGIC) && write_u32(file, CONFIG_VERSION) &&
		       write_u32(file, (uint32_t)count);
	for (size_t i = 0; written && i < count; ++i) {
		const struct ksu_string_pair *entry = entries + i;
		written = write_u32(file, (uint32_t)entry->key_length) &&
			  fwrite(entry->key, 1, entry->key_length, file) == entry->key_length &&
			  write_u32(file, (uint32_t)entry->value_length) &&
			  fwrite(entry->value, 1, entry->value_length, file) == entry->value_length;
	}
	int result = written ? fflush(file) : -1;
	if (!result)
		result = fsync(fileno(file));
	int error = errno;
	if (fclose(file) && !result) {
		result = -1;
		error = errno;
	}
	if (!result) {
		result = rename(temporary, path);
		error = errno;
	}
	free(temporary);
	errno = error;
	return result;
}

int ksu_config_clear(const char *path)
{
	struct stat st;
	return stat(path, &st) ? 0 : unlink(path);
}

int ksu_clear_temporary_configs(const char *root)
{
	struct stat st;
	if (stat(root, &st))
		return 0;
	DIR *dir = opendir(root);
	if (!dir)
		return -1;
	int result = 0;
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(dir);
		if (!entry) {
			result = errno ? -1 : 0;
			break;
		}
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		char *directory = ksu_join_path(root, entry->d_name);
		if (!directory) {
			result = -1;
			break;
		}
		if (stat(directory, &st) || !S_ISDIR(st.st_mode)) {
			free(directory);
			continue;
		}
		char *path = ksu_join_path(directory, "tmp.config");
		free(directory);
		if (!path) {
			result = -1;
			break;
		}
		if (ksu_config_clear(path))
			fprintf(stderr, "Failed to clear temp config %s: %s\n", path,
				strerror(errno));
		free(path);
	}
	int error = errno;
	closedir(dir);
	errno = error;
	return result;
}

static int print_value(const struct ksu_string_pair *entry, bool with_key)
{
	if (with_key && (fwrite(entry->key, 1, entry->key_length, stdout) != entry->key_length ||
			 fputc('=', stdout) == EOF))
		return -1;
	return fwrite(entry->value, 1, entry->value_length, stdout) == entry->value_length &&
		       fputc('\n', stdout) != EOF
		   ? 0
		   : -1;
}

int ksu_config_run(enum ksu_config_command command, const char *directory, int temporary,
		   const unsigned char *key, size_t key_length, const unsigned char *value,
		   size_t value_length)
{
	if (command == KSU_CONFIG_GET || command == KSU_CONFIG_LIST) {
		struct ksu_config_view view;
		if (ksu_config_view_load(directory, &view))
			return -1;
		int result = 0;
		if (command == KSU_CONFIG_GET) {
			const struct ksu_string_pair *entry =
			    ksu_strings_find(&view.merged, key, key_length);
			if (entry)
				result = print_value(entry, false);
			else {
				errno = ENOENT;
				result = -1;
			}
		} else if (!view.merged.count) {
			result = puts("No config entries found") < 0 ? -1 : 0;
		} else {
			for (size_t i = 0; !result && i < view.merged.count; ++i)
				result = print_value(view.merged.entries + i, true);
		}
		ksu_config_view_free(&view);
		return result;
	}
	char *path = ksu_join_path(directory, temporary ? "tmp.config" : "persist.config");
	if (!path)
		return -1;
	struct ksu_string_map config = {0};
	int result;
	if (command == KSU_CONFIG_CLEAR) {
		result = ksu_config_clear(path);
		goto out;
	}
	result = ksu_config_load(path, &config);
	if (result)
		goto out;
	const struct ksu_string_pair *found = ksu_strings_find(&config, key, key_length);
	if (command == KSU_CONFIG_DELETE) {
		if (!found) {
			errno = ENOENT;
			result = -1;
			goto out;
		}
		size_t index = (size_t)(found - config.entries);
		memmove(config.entries + index, config.entries + index + 1,
			(config.count - index - 1) * sizeof(*config.entries));
		--config.count;
	} else {
		size_t index = found ? (size_t)(found - config.entries) : config.count;
		if (!found) {
			struct ksu_string_pair *entries =
			    realloc(config.entries, (config.count + 1) * sizeof(*entries));
			if (!entries) {
				result = -1;
				goto out;
			}
			config.entries = entries;
			++config.count;
		}
		config.entries[index] =
		    (struct ksu_string_pair){key, key_length, value, value_length};
	}
	result = ksu_config_save(directory, path, config.entries, config.count);
out:
	int error = errno;
	ksu_strings_free(&config);
	free(path);
	errno = error;
	return result;
}
