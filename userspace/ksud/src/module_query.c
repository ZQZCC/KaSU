// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "config.h"
#include "file.h"
#include "native.h"
#include "text.h"
#include "yyjson.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct module_config {
	struct ksu_config_view view;
	struct module_config *next;
	char id[];
};

static void free_configs(struct module_config *config)
{
	while (config) {
		struct module_config *next = config->next;
		ksu_config_view_free(&config->view);
		free(config);
		config = next;
	}
}

static int load_configs(const char *root, const char *module_directory,
			struct module_config **configs)
{
	*configs = NULL;
	DIR *dir = opendir(module_directory);
	if (!dir)
		return 0;
	int result = 0;
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		size_t length = strlen(entry->d_name);
		if (!ksu_valid_identifier((unsigned char *)entry->d_name, length))
			continue;
		char *path = ksu_join_path(module_directory, entry->d_name);
		if (!path) {
			result = -1;
			break;
		}
		int usable = ksu_module_active(path);
		free(path);
		if (usable < 0) {
			result = -1;
			break;
		}
		if (!usable)
			continue;
		path = ksu_join_path(root, entry->d_name);
		if (!path) {
			result = -1;
			break;
		}
		struct module_config *config = calloc(1, sizeof(*config) + length + 1);
		if (!config) {
			free(path);
			result = -1;
			break;
		}
		config->next = *configs;
		*configs = config;
		memcpy(config->id, entry->d_name, length + 1);
		result = ksu_config_view_load(path, &config->view);
		free(path);
		if (result)
			break;
	}
	closedir(dir);
	return result;
}

static bool config_true(const struct ksu_string_pair *entry)
{
	return ksu_text_true(entry->value, entry->value_length);
}

static bool set_string(yyjson_mut_doc *doc, yyjson_mut_val *object, const void *key,
		       size_t key_length, const void *value, size_t value_length)
{
	yyjson_mut_val *k = yyjson_mut_strncpy(doc, key, key_length);
	yyjson_mut_val *v = yyjson_mut_strncpy(doc, value, value_length);
	return k && v && yyjson_mut_obj_put(object, k, v);
}

static bool resolve_icon(yyjson_mut_doc *doc, yyjson_mut_val *object, const char *key,
			 const char *directory)
{
	yyjson_mut_val *value = yyjson_mut_obj_get(object, key);
	if (!value)
		return true;
	const unsigned char *text = (const unsigned char *)yyjson_mut_get_str(value);
	size_t length = yyjson_mut_get_len(value);
	ksu_trim_space(&text, &length);
	if (!length || *text == '/' || memchr(text, 0, length))
		return true;
	for (size_t start = 0, end; start < length; start = end + 1) {
		for (end = start; end < length && text[end] != '/'; ++end) {
		}
		if (end - start == 2 && text[start] == '.' && text[start + 1] == '.')
			return true;
	}
	char *relative = strndup((const char *)text, length);
	if (!relative)
		return false;
	char *path = ksu_join_path(directory, relative);
	free(relative);
	if (!path)
		return false;
	struct stat st;
	bool result = true;
	if (!stat(path, &st) && S_ISREG(st.st_mode) && ksu_valid_utf8(path, strlen(path)))
		result = set_string(doc, object, key, strlen(key), path, strlen(path));
	free(path);
	return result;
}

static bool apply_config(yyjson_mut_doc *doc, yyjson_mut_val *object,
			 const struct ksu_string_map *config)
{
	const struct ksu_string_pair *description =
	    ksu_strings_find(config, (unsigned char *)"override.description", 20);
	if (description && !set_string(doc, object, "description", 11, description->value,
				       description->value_length))
		return false;
	size_t length = 0, count = 0;
	for (size_t i = 0; i < config->count; ++i) {
		const struct ksu_string_pair *entry = config->entries + i;
		if (entry->key_length >= 7 && !memcmp(entry->key, "manage.", 7) &&
		    config_true(entry)) {
			length += entry->key_length - 7;
			++count;
		}
	}
	if (!count)
		return true;
	char *features = malloc(length + count);
	if (!features)
		return false;
	char *out = features;
	bool first = true;
	for (size_t i = 0; i < config->count; ++i) {
		const struct ksu_string_pair *entry = config->entries + i;
		if (entry->key_length < 7 || memcmp(entry->key, "manage.", 7) ||
		    !config_true(entry))
			continue;
		if (!first)
			*out++ = ',';
		first = false;
		memcpy(out, entry->key + 7, entry->key_length - 7);
		out += entry->key_length - 7;
	}
	bool result =
	    set_string(doc, object, "managedFeatures", 15, features, (size_t)(out - features));
	free(features);
	return result;
}

static bool apply_module_config(yyjson_mut_doc *doc, yyjson_mut_val *object, yyjson_mut_val *id,
				const char *root)
{
	const unsigned char *name = (const unsigned char *)yyjson_mut_get_str(id);
	if (!ksu_valid_identifier(name, yyjson_mut_get_len(id)))
		return true;
	char *path = ksu_join_path(root, (const char *)name);
	if (!path)
		return false;
	struct stat st;
	struct ksu_config_view view = {0};
	bool valid = true;
	if (!stat(path, &st) && S_ISDIR(st.st_mode))
		valid =
		    !ksu_config_view_load(path, &view) && apply_config(doc, object, &view.merged);
	ksu_config_view_free(&view);
	free(path);
	return valid;
}

int ksu_list_modules(const char *directory, const char *config_directory)
{
	yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
	yyjson_mut_val *array = doc ? yyjson_mut_arr(doc) : NULL;
	DIR *dir = NULL;
	int result = -1;
	if (!array)
		goto out;
	yyjson_mut_doc_set_root(doc, array);
	dir = opendir(directory);
	struct dirent *entry;
	while (dir && (entry = readdir(dir))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		char *path = ksu_join_path(directory, entry->d_name);
		char *property = path ? ksu_join_path(path, "module.prop") : NULL;
		if (!property) {
			free(path);
			goto out;
		}
		struct ksu_string_map properties;
		int parsed = ksu_properties_load(property, &properties);
		free(property);
		if (parsed) {
			free(path);
			continue;
		}
		yyjson_mut_val *object = yyjson_mut_obj(doc);
		bool valid = object != NULL;
		for (size_t i = 0; valid && i < properties.count; ++i) {
			const struct ksu_string_pair *pair = properties.entries + i;
			valid = set_string(doc, object, pair->key, pair->key_length, pair->value,
					   pair->value_length);
		}
		ksu_strings_free(&properties);
		yyjson_mut_val *id = yyjson_mut_obj_get(object, "id");
		if (valid && (!id || !yyjson_mut_get_len(id))) {
			size_t length = strlen(entry->d_name);
			if (!ksu_valid_utf8(entry->d_name, length)) {
				free(path);
				continue;
			}
			valid = set_string(doc, object, "id", 2, entry->d_name, length);
			id = yyjson_mut_obj_get(object, "id");
		}
		int flags = ksu_module_flags(path, ~0u);
		if (flags < 0) {
			free(path);
			continue;
		}
		static const struct {
			const char *key;
			unsigned bit;
		} states[] = {
		    {"enabled", KSU_MOD_ENABLED}, {"update", KSU_MOD_UPDATE},
		    {"remove", KSU_MOD_REMOVE},	  {"web", KSU_MOD_WEB},
		    {"action", KSU_MOD_ACTION},	  {"mount", KSU_MOD_MOUNT},
		};
		for (size_t i = 0; valid && i < sizeof(states) / sizeof(states[0]); ++i) {
			const char *value = ((unsigned)flags & states[i].bit) ? "true" : "false";
			valid = set_string(doc, object, states[i].key, strlen(states[i].key), value,
					   strlen(value));
		}
		valid = valid && resolve_icon(doc, object, "actionIcon", path) &&
			resolve_icon(doc, object, "webuiIcon", path);
		valid = valid && apply_module_config(doc, object, id, config_directory);
		free(path);
		if (!valid)
			goto out;
		if (!yyjson_mut_arr_add_val(array, object))
			goto out;
	}
	size_t length;
	char *json = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, &length);
	if (json) {
		result = fwrite(json, 1, length, stdout) == length && fputc('\n', stdout) != EOF
			     ? 0
			     : -1;
		free(json);
	}
out:
	if (result && !errno)
		errno = ENOMEM;
	if (dir)
		closedir(dir);
	yyjson_mut_doc_free(doc);
	return result;
}

int ksu_feature_modules_run(enum ksu_feature_command command, const char *name, uint64_t value,
			    const char *directory, const char *module_directory,
			    const char *config_directory)
{
	struct module_config *configs = NULL;
	bool needs_modules = command == KSU_FEATURE_SET || command == KSU_FEATURE_LIST ||
			     command == KSU_FEATURE_CHECK || command == KSU_FEATURE_INIT;
	if (needs_modules && load_configs(config_directory, module_directory, &configs)) {
		free_configs(configs);
		configs = NULL;
	}
	size_t capacity = 0, count = 0;
	for (struct module_config *config = configs; config; config = config->next)
		capacity += config->view.merged.count;
	struct ksu_managed_feature *managed = capacity ? calloc(capacity, sizeof(*managed)) : NULL;
	if (capacity && !managed) {
		free_configs(configs);
		return -1;
	}
	for (struct module_config *config = configs; config; config = config->next)
		for (size_t i = 0; i < config->view.merged.count; ++i) {
			const struct ksu_string_pair *entry = config->view.merged.entries + i;
			if (entry->key_length < 7 || memcmp(entry->key, "manage.", 7) ||
			    !config_true(entry))
				continue;
			managed[count++] = (struct ksu_managed_feature){
			    .module = (const unsigned char *)config->id,
			    .module_length = strlen(config->id),
			    .feature = entry->key + 7,
			    .feature_length = entry->key_length - 7,
			};
		}
	int result = ksu_feature_run(command, name, value, directory, managed, count);
	int error = errno;
	free(managed);
	free_configs(configs);
	errno = error;
	return result;
}
