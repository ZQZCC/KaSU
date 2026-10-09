// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "cleanup.h"
#include "file.h"
#include "native.h"
#include "text.h"

#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *read_link(const char *path, size_t capacity)
{
	char *target = malloc(capacity);
	while (target) {
		ssize_t length = readlink(path, target, capacity);
		if (length < 0)
			break;
		if ((size_t)length < capacity) {
			target[length] = 0;
			return target;
		}
		capacity *= 2;
		char *grown = realloc(target, capacity);
		if (!grown)
			break;
		target = grown;
	}
	free(target);
	return NULL;
}

int ksu_is_metamodule(const char *directory)
{
	char *path = ksu_join_path(directory, "module.prop");
	if (!path)
		return false;
	struct ksu_string_map properties;
	int result = ksu_properties_load(path, &properties);
	free(path);
	if (result)
		return false;
	const struct ksu_string_pair *entry =
	    ksu_strings_find(&properties, (const unsigned char *)"metamodule", 10);
	bool is_meta = entry && ksu_text_true(entry->value, entry->value_length);
	ksu_strings_free(&properties);
	return is_meta;
}

char *ksu_find_metamodule(const char *link, const char *module_directory)
{
	struct stat st;
	if (!lstat(link, &st) && S_ISLNK(st.st_mode)) {
		char *target = read_link(link, st.st_size > 0 ? (size_t)st.st_size + 1 : 128);
		if (target && *target != '/') {
			const char *slash = strrchr(link, '/');
			char *parent =
			    slash ? strndup(link, slash == link ? 1 : (size_t)(slash - link))
				  : strdup("");
			char *resolved = parent ? ksu_join_path(parent, target) : NULL;
			free(parent);
			free(target);
			target = resolved;
		}
		if (target && !stat(target, &st) && S_ISDIR(st.st_mode))
			return target;
		free(target);
	}
	DIR *dir = opendir(module_directory);
	if (!dir)
		return NULL;
	struct dirent *entry;
	char *found = NULL;
	while ((entry = readdir(dir))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		char *path = ksu_join_path(module_directory, entry->d_name);
		if (!path)
			break;
		if (!stat(path, &st) && S_ISDIR(st.st_mode) && ksu_is_metamodule(path)) {
			free(found);
			found = path;
		} else
			free(path);
	}
	closedir(dir);
	return found;
}

int ksu_set_metamodule_link(const char *target, const char *link)
{
	struct stat st;
	if (!lstat(link, &st) && ksu_remove_tree(link))
		return -1;
	return symlink(target, link);
}

int ksu_remove_metamodule_link(const char *link)
{
	struct stat st;
	return !lstat(link, &st) && S_ISLNK(st.st_mode) ? unlink(link) : 0;
}

static char *module_id(const char *path)
{
	const char *end = path + strlen(path);
	while (end > path) {
		while (end > path && end[-1] == '/')
			--end;
		const char *begin = end;
		while (begin > path && begin[-1] != '/')
			--begin;
		size_t length = (size_t)(end - begin);
		if (length == 1 && *begin == '.') {
			end = begin;
			continue;
		}
		if (!length || (length == 2 && !memcmp(begin, "..", 2)))
			return NULL;
		return strndup(begin, length);
	}
	return NULL;
}

int ksu_exec_metamodule(enum ksu_meta_command command, const char *shell, const char *link,
			const char *module_directory, const char *value, int *status)
{
	const char *name, *variable;
	switch (command) {
	case KSU_META_MOUNT:
		name = "metamount.sh";
		variable = "MODULE_DIR";
		break;
	case KSU_META_UNINSTALL:
		name = "metauninstall.sh";
		variable = "MODULE_ID";
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	KSU_AUTO_FREE char *directory = ksu_find_metamodule(link, module_directory);
	if (!directory)
		return 0;
	KSU_AUTO_FREE char *path = ksu_join_path(directory, name);
	KSU_AUTO_FREE char *disabled = ksu_join_path(directory, "disable");
	if (!path || !disabled)
		return -1;
	struct stat st;
	if (!stat(disabled, &st)) {
		__android_log_print(ANDROID_LOG_INFO, "KernelSU",
				    "Metamodule is disabled, skipping %s", name);
		return 0;
	}
	if (stat(path, &st))
		return 0;
	KSU_AUTO_FREE char *id = module_id(directory);
	if (id && !ksu_valid_utf8(id, strlen(id))) {
		free(id);
		id = NULL;
	}
	KSU_AUTO_FREE char *extra = NULL;
	if (asprintf(&extra, "%s=%s", variable, value) < 0)
		return -1;
	char *overrides[] = {extra, NULL};
	__android_log_print(ANDROID_LOG_INFO, "KernelSU", "Executing metamodule %s", name);
	int result =
	    ksu_exec_shell(shell, path, 0, command == KSU_META_UNINSTALL ? directory : NULL, id,
			   overrides, status);
	return result ? result : 1;
}

static int file_exists(const char *directory, const char *name)
{
	KSU_AUTO_FREE char *path = ksu_join_path(directory, name);
	if (!path)
		return -1;
	struct stat st;
	return !stat(path, &st);
}

int ksu_install_safety(const char *link, const char *module_directory, const char *update_directory)
{
	KSU_AUTO_FREE char *directory = ksu_find_metamodule(link, module_directory);
	if (!directory)
		return 0;
	KSU_AUTO_FREE char *id = module_id(directory);
	KSU_AUTO_FREE char *updated = id ? ksu_join_path(update_directory, id) : NULL;
	int install = file_exists(directory, "metainstall.sh");
	if (install < 0 || (id && !updated))
		return -1;
	if (!install && updated) {
		install = file_exists(updated, "metainstall.sh");
		if (install < 0)
			return -1;
	}
	if (!install)
		return 0;
	int update = file_exists(directory, "update");
	int remove = file_exists(directory, "remove");
	int disabled = file_exists(directory, "disable");
	if (update < 0 || remove < 0 || disabled < 0)
		return -1;
	if (disabled && !update && !remove)
		return 1;
	return update || remove ? 2 : 0;
}

static char *default_installer(const char *library)
{
	char *script;
	return asprintf(&script, "%s\ninstall_module\nexit 0\n", library) < 0 ? NULL : script;
}

char *ksu_installer_script(const char *link, const char *module_directory, const char *library,
			   int is_metamodule)
{
	if (is_metamodule) {
		__android_log_print(ANDROID_LOG_INFO, "KernelSU",
				    "Installing metamodule, using default installer");
		return default_installer(library);
	}
	KSU_AUTO_FREE char *directory = ksu_find_metamodule(link, module_directory);
	if (!directory) {
		__android_log_print(ANDROID_LOG_INFO, "KernelSU",
				    "No metamodule found, using default installer");
		return default_installer(library);
	}
	KSU_AUTO_FREE char *path = ksu_join_path(directory, "metainstall.sh");
	int disabled = file_exists(directory, "disable");
	if (!path || disabled < 0)
		return NULL;
	struct stat st;
	if (disabled || stat(path, &st)) {
		__android_log_print(ANDROID_LOG_INFO, "KernelSU", "%s, using default installer",
				    disabled ? "Metamodule is disabled"
					     : "Metamodule exists but has no metainstall.sh");
		return default_installer(library);
	}
	size_t length;
	__android_log_print(ANDROID_LOG_INFO, "KernelSU", "Using metainstall.sh from metamodule");
	KSU_AUTO_FREE unsigned char *data = ksu_read_file(path, &length);
	if (!data)
		return NULL;
	if (!ksu_valid_utf8(data, length)) {
		errno = EILSEQ;
		return NULL;
	}
	if (memchr(data, 0, length)) {
		errno = EINVAL;
		return NULL;
	}
	size_t prefix = strlen(library);
	char *script = malloc(prefix + length + sizeof("\nexit 0\n") + 1);
	if (!script)
		return NULL;
	memcpy(script, library, prefix);
	script[prefix++] = '\n';
	memcpy(script + prefix, data, length);
	memcpy(script + prefix + length, "\nexit 0\n", sizeof("\nexit 0\n"));
	return script;
}
