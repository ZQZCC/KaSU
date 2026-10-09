// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "file.h"
#include "native.h"
#include <dirent.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int ksu_module_mark(const char *path, enum ksu_module_state state)
{
	struct stat st;
	if (stat(path, &st))
		return -1;
	const char *name =
	    state == KSU_MODULE_ENABLE || state == KSU_MODULE_DISABLE ? "disable" : "remove";
	char *marker = ksu_join_path(path, name);
	if (!marker)
		return -1;
	int result;
	if (state == KSU_MODULE_ENABLE || state == KSU_MODULE_UNDO_REMOVE) {
		result = stat(marker, &st) ? 0 : unlink(marker) ? -1 : 1;
	} else {
		int flags = O_WRONLY | O_CREAT | O_CLOEXEC;
		flags |= state == KSU_MODULE_REMOVE ? O_TRUNC : O_EXCL;
		int fd = open(marker, flags, 0666);
		if (fd >= 0) {
			close(fd);
			result = 1;
		} else if (state == KSU_MODULE_DISABLE && errno == EEXIST && !stat(marker, &st) &&
			   S_ISREG(st.st_mode)) {
			result = 1;
		} else {
			result = -1;
		}
	}
	int error = errno;
	free(marker);
	errno = error;
	return result;
}

int ksu_module_flags(const char *path, unsigned mask)
{
	static const struct {
		const char *name;
		unsigned bit;
	} markers[] = {
	    {"update", KSU_MOD_UPDATE},
	    {"remove", KSU_MOD_REMOVE},
	    {"webroot", KSU_MOD_WEB},
	    {"action.sh", KSU_MOD_ACTION},
	};
	size_t prefix = strlen(path) + 1;
	char *file = malloc(prefix + sizeof("skip_mount"));
	if (!file)
		return -1;
	memcpy(file, path, prefix - 1);
	file[prefix - 1] = '/';
	struct stat st;
	unsigned flags = 0;
	if (mask & KSU_MOD_ENABLED) {
		strcpy(file + prefix, "disable");
		if (stat(file, &st))
			flags |= KSU_MOD_ENABLED;
	}
	for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i) {
		if (!(mask & markers[i].bit))
			continue;
		strcpy(file + prefix, markers[i].name);
		if (!stat(file, &st))
			flags |= markers[i].bit;
	}
	if (mask & KSU_MOD_MOUNT) {
		strcpy(file + prefix, "system");
		if (!stat(file, &st)) {
			strcpy(file + prefix, "skip_mount");
			if (stat(file, &st))
				flags |= KSU_MOD_MOUNT;
		}
	}
	free(file);
	return (int)flags;
}

int ksu_module_active(const char *path)
{
	struct stat st;
	if (stat(path, &st) || !S_ISDIR(st.st_mode))
		return 0;
	size_t prefix = strlen(path) + 1;
	char *marker = malloc(prefix + sizeof("disable"));
	if (!marker)
		return -1;
	memcpy(marker, path, prefix - 1);
	marker[prefix - 1] = '/';
	strcpy(marker + prefix, "disable");
	int active = stat(marker, &st) != 0;
	if (active) {
		strcpy(marker + prefix, "remove");
		active = stat(marker, &st) != 0;
	}
	free(marker);
	return active;
}

int ksu_apply_module_update(const char *updated_path, const char *module_path)
{
	struct stat st;
	int flags = KSU_MOD_ENABLED;
	if (!stat(module_path, &st)) {
		flags = ksu_module_flags(module_path, KSU_MOD_ENABLED | KSU_MOD_REMOVE);
		if (flags < 0 || ksu_remove_tree(module_path))
			return -1;
	}
	if (rename(updated_path, module_path))
		return -1;
	const char *name = flags & KSU_MOD_REMOVE	? "remove"
			   : !(flags & KSU_MOD_ENABLED) ? "disable"
							: NULL;
	if (name) {
		char *marker = ksu_join_path(module_path, name);
		if (!marker)
			return -1;
		if (ksu_ensure_file(marker))
			fprintf(stderr, "Failed to create %s: %s\n", marker, strerror(errno));
		free(marker);
	}
	return 0;
}

int ksu_apply_module_updates(const char *update_directory, const char *module_directory)
{
	DIR *dir = opendir(update_directory);
	if (!dir)
		return -1;
	int result = 0;
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		char *update = ksu_join_path(update_directory, entry->d_name);
		char *module = ksu_join_path(module_directory, entry->d_name);
		if (!update || !module) {
			result = -1;
		} else {
			struct stat st;
			if (!stat(update, &st) && S_ISDIR(st.st_mode))
				result = ksu_apply_module_update(update, module);
		}
		free(update);
		free(module);
		if (result < 0)
			break;
	}
	int error = errno;
	closedir(dir);
	errno = error;
	return result;
}
