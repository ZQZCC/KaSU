// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "file.h"
#include "native.h"
#include "text.h"

#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

static void clear_configs(const char *root, const char *id)
{
	if (!ksu_valid_identifier((const unsigned char *)id, strlen(id))) {
		__android_log_print(ANDROID_LOG_WARN, "KernelSU",
				    "Failed to clear configs: invalid ID %s", id);
		return;
	}
	char *path = ksu_join_path(root, id);
	struct stat st;
	if (!path || (!stat(path, &st) && ksu_remove_tree(path)))
		__android_log_print(ANDROID_LOG_WARN, "KernelSU",
				    "Failed to clear configs for %s: %s", id, strerror(errno));
	free(path);
}

static void prune_module(const char *shell, const char *path, const char *id, const char *modules,
			 const char *link, const char *configs)
{
	__android_log_print(ANDROID_LOG_INFO, "KernelSU", "remove module: %s", path);
	if (ksu_is_metamodule(path)) {
		char *trimmed_link = strdup(link);
		if (trimmed_link) {
			size_t length = strlen(trimmed_link);
			while (length && trimmed_link[length - 1] == '/')
				trimmed_link[--length] = 0;
		}
		if (!trimmed_link || ksu_remove_metamodule_link(trimmed_link))
			__android_log_print(ANDROID_LOG_WARN, "KernelSU",
					    "Failed to remove metamodule symlink: %s",
					    strerror(errno));
		free(trimmed_link);
	} else {
		int status = 0;
		int result =
		    ksu_exec_metamodule(KSU_META_UNINSTALL, shell, link, modules, id, &status);
		if (result < 0 || (result && (!WIFEXITED(status) || WEXITSTATUS(status))))
			__android_log_print(ANDROID_LOG_WARN, "KernelSU",
					    "Failed to exec metamodule uninstall for %s", id);
	}
	char *uninstaller = ksu_join_path(path, "uninstall.sh");
	struct stat st;
	const char *environment_id =
	    ksu_valid_identifier((const unsigned char *)id, strlen(id)) ? id : NULL;
	if (!uninstaller ||
	    (!stat(uninstaller, &st) &&
	     ksu_exec_script(shell, uninstaller, path, environment_id, NULL, -1) < 0))
		__android_log_print(ANDROID_LOG_WARN, "KernelSU", "Failed to exec uninstaller: %s",
				    strerror(errno));
	free(uninstaller);
	clear_configs(configs, id);
	if (ksu_remove_tree(path))
		__android_log_print(ANDROID_LOG_WARN, "KernelSU", "Failed to remove %s: %s", path,
				    strerror(errno));
}

int ksu_prune_modules(const char *shell, const char *module_directory, const char *metamodule_link,
		      const char *config_directory)
{
	DIR *dir = opendir(module_directory);
	if (!dir)
		return -1;
	struct dirent *entry;
	int result = 0;
	while ((entry = readdir(dir))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		char *path = ksu_join_path(module_directory, entry->d_name);
		if (!path) {
			result = -1;
			break;
		}
		struct stat st;
		if (!stat(path, &st) && S_ISDIR(st.st_mode)) {
			int flags = ksu_module_flags(path, KSU_MOD_REMOVE);
			if (flags < 0)
				result = -1;
			else if (flags & KSU_MOD_REMOVE) {
				const char *id =
				    ksu_valid_utf8(entry->d_name, strlen(entry->d_name))
					? entry->d_name
					: "";
				prune_module(shell, path, id, module_directory, metamodule_link,
					     config_directory);
			}
		}
		free(path);
		if (result < 0)
			break;
	}
	int error = errno;
	closedir(dir);
	if (!result) {
		dir = opendir(module_directory);
		if (!dir)
			return -1;
		int remaining = 0;
		while ((entry = readdir(dir))) {
			if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
				continue;
			char *path = ksu_join_path(module_directory, entry->d_name);
			char *prop = path ? ksu_join_path(path, "module.prop") : NULL;
			struct stat st;
			if (!path || !prop)
				result = -1;
			else
				remaining = !stat(prop, &st);
			free(prop);
			free(path);
			if (result < 0 || remaining)
				break;
		}
		if (!result && !remaining)
			__android_log_print(ANDROID_LOG_INFO, "KernelSU", "no remaining modules.");
		error = errno;
		closedir(dir);
	}
	errno = error;
	return result;
}
