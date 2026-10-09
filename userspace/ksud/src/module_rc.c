// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "file.h"
#include "native.h"
#include "text.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct rc_module {
	char *path;
	const char *id;
	bool disabled;
};

static int compare_entry(const struct dirent **a, const struct dirent **b)
{
	return strcmp((*a)->d_name, (*b)->d_name);
}

static int rc_entry(const struct dirent *entry)
{
	size_t length = strlen(entry->d_name);
	return length > 3 && !strcmp(entry->d_name + length - 3, ".rc");
}

static int compare_module(const void *a, const void *b)
{
	return strcmp(((const struct rc_module *)a)->id, ((const struct rc_module *)b)->id);
}

static int write_path(FILE *file, const char *path)
{
	const unsigned char *cursor = (const unsigned char *)path, *end = cursor + strlen(path);
	while (cursor < end) {
		unsigned char bytes[4];
		size_t length = ksu_utf8_put(bytes, ksu_utf8_next(&cursor, end));
		if (fwrite(bytes, 1, length, file) != length)
			return -1;
	}
	return 0;
}

static int collect_rc(const char *directory, const char *module_id, FILE *out)
{
	struct dirent **entries;
	int count = scandir(directory, &entries, rc_entry, compare_entry);
	if (count < 0)
		return 0;
	int result = 0;
	for (int i = 0; !result && i < count; ++i) {
		const char *name = entries[i]->d_name;
		char *path = ksu_join_path(directory, name);
		if (!path) {
			result = -1;
			break;
		}
		struct stat st;
		if (lstat(path, &st) || !S_ISREG(st.st_mode) ||
		    (!module_id && !(st.st_mode & 0111))) {
			free(path);
			continue;
		}
		size_t size;
		unsigned char *data = ksu_read_file(path, &size);
		if (!data) {
			free(path);
			result = -1;
			break;
		}
		if (fputs("# === from ", out) == EOF ||
		    (module_id && fprintf(out, "%s:", module_id) < 0) || write_path(out, path) ||
		    fputs(" ===\n", out) == EOF || fwrite(data, 1, size, out) != size ||
		    fputc('\n', out) == EOF)
			result = -1;
		free(data);
		free(path);
	}
	for (int i = 0; i < count; ++i)
		free(entries[i]);
	free(entries);
	return result;
}

static int collect_modules(const char *directory, struct rc_module **modules, size_t *count)
{
	DIR *dir = opendir(directory);
	if (!dir)
		return 0;
	struct dirent *entry;
	size_t capacity = *count;
	int result = 0;
	while ((entry = readdir(dir))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") ||
		    !ksu_valid_utf8(entry->d_name, strlen(entry->d_name)))
			continue;
		char *path = ksu_join_path(directory, entry->d_name);
		if (!path) {
			result = -1;
			break;
		}
		struct stat st;
		if (stat(path, &st) || !S_ISDIR(st.st_mode)) {
			free(path);
			continue;
		}
		int active = ksu_module_active(path);
		if (active < 0) {
			free(path);
			result = -1;
			break;
		}
		size_t i;
		for (i = 0; i < *count; ++i)
			if (!strcmp((*modules)[i].id, entry->d_name))
				break;
		if (i != *count) {
			if (!active)
				(*modules)[i].disabled = true;
			free(path);
			continue;
		}
		if (*count == capacity) {
			if (capacity > SIZE_MAX / 2 / sizeof(**modules)) {
				errno = EOVERFLOW;
				free(path);
				result = -1;
				break;
			}
			capacity = capacity ? capacity * 2 : 16;
			struct rc_module *list = realloc(*modules, capacity * sizeof(*list));
			if (!list) {
				free(path);
				result = -1;
				break;
			}
			*modules = list;
		}
		(*modules)[(*count)++] = (struct rc_module){path, strrchr(path, '/') + 1, !active};
	}
	closedir(dir);
	return result;
}

int ksu_regenerate_rc(const char *common_directory, const char *module_directory,
		      const char *update_directory, const char *preinit_directory)
{
	if (ksu_mkdirs(preinit_directory))
		return -1;
	char *temporary = ksu_join_path(preinit_directory, ".modules.rc.tmp");
	char *output = ksu_join_path(preinit_directory, "modules.rc");
	FILE *file = temporary ? fopen(temporary, "we") : NULL;
	struct rc_module *modules = NULL;
	size_t count = 0;
	int result = -1;
	if (!file || !output)
		goto out;
	if (collect_rc(common_directory, NULL, file) ||
	    collect_modules(update_directory, &modules, &count) ||
	    collect_modules(module_directory, &modules, &count))
		goto out;
	if (count > 1)
		qsort(modules, count, sizeof(*modules), compare_module);
	for (size_t i = 0; i < count; ++i) {
		if (modules[i].disabled)
			continue;
		char *directory = ksu_join_path(modules[i].path, "initrc");
		if (!directory)
			goto out;
		int collected = collect_rc(directory, modules[i].id, file);
		free(directory);
		if (collected)
			goto out;
	}
	result = fflush(file);
	if (!result)
		result = fsync(fileno(file));
	if (!result) {
		fclose(file);
		file = NULL;
		result = rename(temporary, output);
	}
out:
	int error = errno;
	if (file)
		fclose(file);
	for (size_t i = 0; i < count; ++i)
		free(modules[i].path);
	free(modules);
	free(temporary);
	free(output);
	errno = error;
	return result;
}
