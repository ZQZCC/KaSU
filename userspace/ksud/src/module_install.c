// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "file.h"
#include "native.h"
#include "text.h"

#include <android/log.h>
#include <errno.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void format_size(char text[32], uint64_t bytes)
{
	static const char *units[] = {"B", "kB", "MB", "GB", "TB", "PB", "EB"};
	double size = (double)bytes;
	size_t unit = 0;
	while (size >= 1000) {
		size /= 1000;
		unit++;
	}
	int decimals = size - (double)(uint64_t)size <= DBL_EPSILON ? 0 : 2;
	snprintf(text, 32, "%.*f %s", decimals, size, units[unit]);
}

static void print_border(const char *corner, unsigned length)
{
	fputs(corner, stdout);
	for (unsigned i = 0; i < length; i++)
		fputs("\u2500", stdout);
	putchar('\n');
}

static int install_error(char **message, const char *text, int code)
{
	*message = strdup(text);
	errno = code;
	return -1;
}

static int check_metamodule(const char *link, const char *modules, const char *id, char **message)
{
	char *existing = ksu_find_metamodule(link, modules);
	if (!existing)
		return 0;
	char *path = ksu_join_path(existing, "module.prop");
	free(existing);
	if (!path)
		return -1;
	struct ksu_string_map properties;
	ksu_properties_load(path, &properties);
	free(path);
	const struct ksu_string_pair *entry =
	    ksu_strings_find(&properties, (const unsigned char *)"id", 2);
	const unsigned char *value = entry ? entry->value : (const unsigned char *)"unknown";
	size_t size = entry ? entry->value_length : 7;
	int result = 0;
	if (size != strlen(id) || memcmp(value, id, size)) {
		puts("\n\u274c Installation Failed");
		print_border("\u250c", 32);
		fputs("\u2502 A metamodule is already installed\n\u2502   Current metamodule: ",
		      stdout);
		fwrite(value, 1, size, stdout);
		puts("\n\u2502\n\u2502 Only one metamodule can be active at a time.\n\u2502\n"
		     "\u2502 To install this metamodule:\n\u2502   1. Uninstall the current "
		     "metamodule\n"
		     "\u2502   2. Reboot your device\n\u2502   3. Install the new metamodule");
		print_border("\u2514", 33);
		putchar('\n');
		result = install_error(message, "Cannot install multiple metamodules", EEXIST);
	}
	ksu_strings_free(&properties);
	return result;
}

static int check_safety(const char *link, const char *modules, const char *updates, char **message)
{
	int state = ksu_install_safety(link, modules, updates);
	if (!state)
		return 0;
	puts("\n\u274c Installation Blocked");
	print_border("\u250c", 32);
	puts("\u2502 A metamodule with custom installer is active\n\u2502");
	if (state == 1)
		puts("\u2502 Current state: Disabled\n"
		     "\u2502 Action required: Re-enable or uninstall it, then reboot");
	else
		puts("\u2502 Current state: Pending changes\n"
		     "\u2502 Action required: Reboot to apply changes first");
	print_border("\u2514", 33);
	putchar('\n');
	return install_error(message, "Metamodule installation blocked", EBUSY);
}

int ksu_install_module(const char *zip, const char *shell, const char *module_directory,
		       const char *update_directory, const char *metamodule_link,
		       const char *library, char **message)
{
	struct ksu_string_map properties = {0};
	char *id = NULL, *updated = NULL, *module = NULL, *system = NULL;
	char *source = NULL, *destination = NULL, *marker = NULL, *script = NULL, *zip_env = NULL;
	int result = -1;
	*message = NULL;
	struct ksu_zip *archive = ksu_zip_open(zip);
	if (!archive || ksu_zip_properties(archive, &properties))
		goto done;
	const struct ksu_string_pair *entry =
	    ksu_strings_find(&properties, (const unsigned char *)"id", 2);
	if (!entry) {
		install_error(message, "module id not found in module.prop!", EINVAL);
		goto done;
	}
	const unsigned char *value = entry->value;
	size_t length = entry->value_length;
	ksu_trim_space(&value, &length);
	if (!ksu_valid_identifier(value, length)) {
		if (asprintf(message, "Invalid module ID in module.prop: '%.*s'", (int)length,
			     value) < 0)
			goto done;
		errno = EINVAL;
		goto done;
	}
	id = strndup((const char *)value, length);
	if (!id)
		goto done;
	entry = ksu_strings_find(&properties, (const unsigned char *)"metamodule", 10);
	bool is_meta = entry && ksu_text_true(entry->value, entry->value_length);
	ksu_strings_free(&properties);
	if (is_meta ? check_metamodule(metamodule_link, module_directory, id, message)
		    : check_safety(metamodule_link, module_directory, update_directory, message))
		goto done;
	uint64_t bytes;
	if (ksu_zip_size(archive, &bytes))
		goto done;
	char size[32];
	format_size(size, bytes);
	printf("- Module size: %s\n", size);
	__android_log_print(ANDROID_LOG_INFO, "KernelSU", "zip uncompressed size: %s", size);
	if (ksu_mkdirs(update_directory) || ksu_setsyscon(update_directory))
		goto done;
	updated = ksu_join_path(update_directory, id);
	if (!updated)
		goto done;
	printf("- Installing to %s\n", updated);
	struct stat st;
	if (!stat(updated, &st) && ksu_remove_tree(updated))
		goto done;
	if (ksu_mkdirs(updated))
		goto done;
	puts("- Extracting module files");
	fflush(stdout);
	if (ksu_zip_extract(archive, updated))
		goto done;
	ksu_zip_close(archive);
	archive = NULL;
	system = ksu_join_path(updated, "system");
	if (!system)
		goto done;
	if (!stat(system, &st) && (chmod(system, 0755) || ksu_restore_syscon(system, 0)))
		goto done;
	puts("- Running module installer");
	script = ksu_installer_script(metamodule_link, module_directory, library, is_meta);
	if (!script || asprintf(&zip_env, "ZIPFILE=%s", zip) < 0)
		goto done;
	char *environment[] = {"OUTFD=1", zip_env, NULL};
	int status;
	fflush(stdout);
	if (ksu_exec_shell(shell, script, 1, NULL, id, environment, &status))
		goto done;
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		install_error(message, "Failed to install module script", ECANCELED);
		goto done;
	}
	module = ksu_join_path(module_directory, id);
	source = ksu_join_path(updated, "module.prop");
	destination = module ? ksu_join_path(module, "module.prop") : NULL;
	marker = module ? ksu_join_path(module, "update") : NULL;
	if (!module || !source || !destination || !marker || ksu_mkdirs(module) ||
	    ksu_copy_file(source, destination) || ksu_ensure_file(marker))
		goto done;
	if (is_meta) {
		puts("- Creating metamodule symlink");
		size_t length = strlen(metamodule_link);
		while (length && metamodule_link[length - 1] == '/')
			length--;
		char *link = strndup(metamodule_link, length);
		if (!link)
			goto done;
		int linked = ksu_set_metamodule_link(module, link);
		int error = errno;
		free(link);
		errno = error;
		if (linked)
			goto done;
	}
	puts("- Module installed successfully!");
	__android_log_print(ANDROID_LOG_INFO, "KernelSU", "Module %s installed successfully!", id);
	result = 0;
done:
	int error = errno;
	fflush(stdout);
	free(zip_env);
	free(script);
	free(marker);
	free(destination);
	free(source);
	free(system);
	free(module);
	free(updated);
	free(id);
	ksu_strings_free(&properties);
	ksu_zip_close(archive);
	errno = error;
	return result;
}
