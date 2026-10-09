// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "driver.h"
#include "file.h"
#include "native.h"
#include "uapi/supercall.h"

#include <android/log.h>
#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FEATURE_MAGIC 0x7f4b5355
#define FEATURE_VERSION 1

static const struct {
	const char *name, *description;
} features[] = {
    {"su_compat",
     "SU Compatibility Mode - allows authorized apps to gain root via traditional 'su' command"},
    {"kernel_umount",
     "Kernel Umount - controls whether kernel automatically unmounts modules when not needed"},
    {"sulog", "SU Log - streams kernel sulog events to userspace and persists them to disk"},
    {"adb_root", "ADB Root - Enable adbd root"},
    {"selinux_hide", "SELinux Hide - sanitize /sys/fs/selinux access results for app UIDs"},
};

struct feature_value {
	uint32_t id;
	uint64_t value;
};
struct feature_config {
	struct feature_value *entries;
	size_t count;
};

static int feature_id(const unsigned char *name, size_t length)
{
	for (size_t i = 0; i < sizeof(features) / sizeof(features[0]); ++i)
		if ((length == strlen(features[i].name) &&
		     !memcmp(name, features[i].name, length)) ||
		    (length == 1 && *name == '0' + i))
			return (int)i;
	return -1;
}

static bool manages(const struct ksu_managed_feature *entry, uint32_t id)
{
	return entry->feature_length == strlen(features[id].name) &&
	       !memcmp(entry->feature, features[id].name, entry->feature_length);
}

static int get_feature(uint32_t id, uint64_t *value, bool *supported)
{
	struct ksu_get_feature_cmd cmd = {.feature_id = id};
	if (ksu_ioctl(KSU_IOCTL_GET_FEATURE, &cmd) < 0)
		return -1;
	*value = cmd.value;
	*supported = cmd.supported != 0;
	return 0;
}

static int set_feature(uint32_t id, uint64_t value)
{
	struct ksu_set_feature_cmd cmd = {.feature_id = id, .value = value};
	if (ksu_ioctl(KSU_IOCTL_SET_FEATURE, &cmd) < 0)
		return -1;
	if (id == 2 && value)
		ksu_ensure_sulogd();
	return 0;
}

static int load_config(const char *path, struct feature_config *config)
{
	*config = (struct feature_config){0};
	struct stat st;
	if (stat(path, &st)) {
		__android_log_print(ANDROID_LOG_INFO, "KernelSU",
				    "Feature config not found, using defaults");
		return 0;
	}
	size_t length;
	unsigned char *data = ksu_read_file(path, &length);
	if (!data)
		return -1;
	uint32_t header[3];
	if (length < sizeof(header))
		goto invalid;
	memcpy(header, data, sizeof(header));
	if (le32toh(header[0]) != FEATURE_MAGIC)
		goto invalid;
	if (le32toh(header[1]) != FEATURE_VERSION)
		__android_log_print(ANDROID_LOG_WARN, "KernelSU",
				    "Feature config version mismatch: expected %u, got %u",
				    FEATURE_VERSION, le32toh(header[1]));
	uint32_t count = le32toh(header[2]);
	if (count > (length - sizeof(header)) / 12)
		goto invalid;
	if (count) {
		config->entries = calloc(count, sizeof(*config->entries));
		if (!config->entries)
			goto error;
	}
	for (uint32_t i = 0; i < count; ++i) {
		uint32_t id;
		uint64_t value;
		memcpy(&id, data + 12 + (size_t)i * 12, sizeof(id));
		memcpy(&value, data + 16 + (size_t)i * 12, sizeof(value));
		id = le32toh(id);
		value = le64toh(value);
		size_t index = 0;
		while (index < config->count && config->entries[index].id != id)
			++index;
		config->entries[index] = (struct feature_value){id, value};
		if (index == config->count)
			++config->count;
	}
	free(data);
	__android_log_print(ANDROID_LOG_INFO, "KernelSU", "Loaded %zu features from config",
			    config->count);
	return 0;
invalid:
	errno = EINVAL;
error:
	int error = errno;
	free(data);
	free(config->entries);
	*config = (struct feature_config){0};
	errno = error;
	return -1;
}

static int save_config(const char *directory, const char *path, const struct feature_config *config)
{
	if (ksu_mkdirs(directory))
		return -1;
	FILE *file = fopen(path, "we");
	if (!file)
		return -1;
	uint32_t header[] = {htole32(FEATURE_MAGIC), htole32(FEATURE_VERSION),
			     htole32((uint32_t)config->count)};
	bool written = fwrite(header, sizeof(header), 1, file) == 1;
	for (size_t i = 0; written && i < config->count; ++i) {
		uint32_t id = htole32(config->entries[i].id);
		uint64_t value = htole64(config->entries[i].value);
		written = fwrite(&id, sizeof(id), 1, file) == 1 &&
			  fwrite(&value, sizeof(value), 1, file) == 1;
	}
	int result = written ? fflush(file) : -1;
	if (!result)
		result = fsync(fileno(file));
	int error = errno;
	if (fclose(file) && !result) {
		result = -1;
		error = errno;
	}
	errno = error;
	if (!result)
		__android_log_print(ANDROID_LOG_INFO, "KernelSU", "Saved %zu features to config",
				    config->count);
	return result;
}

static void apply_config(const struct feature_config *config)
{
	__android_log_print(ANDROID_LOG_INFO, "KernelSU",
			    "Applying feature configuration to kernel...");
	size_t applied = 0;
	for (size_t i = 0; i < config->count; ++i) {
		const struct feature_value *entry = config->entries + i;
		char number[11];
		snprintf(number, sizeof(number), "%u", entry->id);
		const char *label = entry->id < sizeof(features) / sizeof(features[0])
					? features[entry->id].name
					: number;
		if (!set_feature(entry->id, entry->value)) {
			++applied;
			__android_log_print(ANDROID_LOG_INFO, "KernelSU",
					    "Set feature %s to %" PRIu64, label, entry->value);
		} else
			__android_log_print(ANDROID_LOG_WARN, "KernelSU",
					    "Failed to set feature %s: %s", label, strerror(errno));
	}
	__android_log_print(ANDROID_LOG_INFO, "KernelSU", "Applied %zu features successfully",
			    applied);
}

static void print_value(uint32_t id, uint64_t value, bool configured)
{
	printf("Feature: %s (%u)\nDescription: %s\n", features[id].name, id,
	       features[id].description);
	if (configured)
		printf("Value: %" PRIu64 "\nStatus: %s\n", value, value ? "enabled" : "disabled");
	else
		puts("Not set in config");
}

static void print_managers(FILE *file, const struct ksu_managed_feature *managed, size_t count,
			   uint32_t id)
{
	bool first = true;
	for (size_t i = 0; i < count; ++i) {
		if (!manages(managed + i, id))
			continue;
		if (!first)
			fputs(", ", file);
		fwrite(managed[i].module, 1, managed[i].module_length, file);
		first = false;
	}
}

static int save_kernel_features(const char *directory, const char *path)
{
	struct feature_value states[sizeof(features) / sizeof(features[0])];
	struct feature_config config = {.entries = states};
	for (uint32_t i = 0; i < sizeof(features) / sizeof(features[0]); ++i) {
		bool supported;
		uint64_t value;
		if (!get_feature(i, &value, &supported) && supported) {
			states[config.count++] = (struct feature_value){i, value};
			__android_log_print(ANDROID_LOG_INFO, "KernelSU",
					    "Saved feature %s = %" PRIu64, features[i].name, value);
		}
	}
	int result = save_config(directory, path, &config);
	if (!result)
		printf("Current feature states saved to config file (%zu features)\n",
		       config.count);
	return result;
}

int ksu_feature_run(enum ksu_feature_command command, const char *name, uint64_t value,
		    const char *directory, const struct ksu_managed_feature *managed, size_t count)
{
	int id = name ? feature_id((const unsigned char *)name, strlen(name)) : -1;
	if (name && id < 0) {
		fprintf(stderr, "Unknown feature: %s\n", name);
		errno = EINVAL;
		return -1;
	}
	bool managed_feature = false, authorized = false;
	const char *caller = getenv("KSU_MODULE");
	for (size_t i = 0; id >= 0 && i < count; ++i) {
		if (manages(managed + i, (uint32_t)id)) {
			managed_feature = true;
			if (caller && *caller && strlen(caller) == managed[i].module_length &&
			    !memcmp(caller, managed[i].module, managed[i].module_length))
				authorized = true;
		}
	}
	if (command == KSU_FEATURE_SET) {
		if (managed_feature && !authorized) {
			fprintf(stderr,
				"Feature '%s' is managed by module(s): ", features[id].name);
			print_managers(stderr, managed, count, (uint32_t)id);
			fputs(". Direct modification is not allowed.\n", stderr);
			errno = EPERM;
			return -1;
		}
		if (managed_feature)
			__android_log_print(ANDROID_LOG_INFO, "KernelSU",
					    "Module '%s' is setting managed feature '%s'", caller,
					    features[id].name);
		if (set_feature((uint32_t)id, value))
			return -1;
		printf("Feature '%s' set to %" PRIu64 " (%s)\n", features[id].name, value,
		       value ? "enabled" : "disabled");
		return 0;
	}
	if (command == KSU_FEATURE_GET || command == KSU_FEATURE_CHECK) {
		if (command == KSU_FEATURE_CHECK && managed_feature) {
			puts("managed");
			return 0;
		}
		bool supported;
		if (get_feature((uint32_t)id, &value, &supported))
			return -1;
		if (command == KSU_FEATURE_CHECK)
			puts(supported ? "supported" : "unsupported");
		else if (!supported)
			printf("Feature '%s' is not supported by kernel\n", name);
		else
			print_value((uint32_t)id, value, true);
		return 0;
	}
	if (command == KSU_FEATURE_LIST) {
		puts("Available Features:");
		for (unsigned i = 0; i < 80; ++i)
			putchar('=');
		putchar('\n');
		for (uint32_t i = 0; i < sizeof(features) / sizeof(features[0]); ++i) {
			bool supported = false, owned = false;
			value = 0;
			(void)get_feature(i, &value, &supported);
			for (size_t j = 0; j < count; ++j)
				owned |= manages(managed + j, i);
			if (!supported)
				fputs("[NOT_SUPPORTED]", stdout);
			else if (value)
				printf("[ENABLED (%" PRIu64 ")]", value);
			else
				fputs("[DISABLED]", stdout);
			printf(" %s (ID=%u)%s\n    %s\n", features[i].name, i,
			       owned ? " [MODULE_MANAGED]" : "", features[i].description);
			if (owned) {
				fputs("    \xe2\x9a\xa0\xef\xb8\x8f  Managed by module(s): ",
				      stdout);
				print_managers(stdout, managed, count, i);
				puts(" (forced to 0 on initialization)");
			}
			putchar('\n');
		}
		return 0;
	}
	char *path = ksu_join_path(directory, ".feature_config");
	if (!path)
		return -1;
	struct feature_config config = {0};
	int result = 0;
	if (command == KSU_FEATURE_INIT)
		__android_log_print(ANDROID_LOG_INFO, "KernelSU",
				    "Initializing features from config...");
	if (command == KSU_FEATURE_SAVE) {
		result = save_kernel_features(directory, path);
		goto done;
	}
	if (load_config(path, &config)) {
		result = -1;
		goto done;
	}
	if (command == KSU_FEATURE_GET_CONFIG) {
		bool configured = false;
		for (size_t i = 0; i < config.count; ++i)
			if (config.entries[i].id == (uint32_t)id) {
				value = config.entries[i].value;
				configured = true;
				break;
			}
		print_value((uint32_t)id, value, configured);
	} else if (command == KSU_FEATURE_LOAD) {
		if (!config.count)
			puts("No features found in config file");
		else {
			apply_config(&config);
			puts("Feature configuration loaded and applied");
		}
	} else if (command == KSU_FEATURE_INIT) {
		for (size_t i = 0; i < count; ++i) {
			int owned = feature_id(managed[i].feature, managed[i].feature_length);
			if (owned < 0) {
				__android_log_print(ANDROID_LOG_WARN, "KernelSU",
						    "Unknown managed feature '%.*s', ignoring",
						    (int)managed[i].feature_length,
						    managed[i].feature);
				continue;
			}
			for (size_t j = 0; j < config.count; ++j)
				if (config.entries[j].id == (uint32_t)owned) {
					memmove(config.entries + j, config.entries + j + 1,
						(--config.count - j) * sizeof(*config.entries));
					__android_log_print(
					    ANDROID_LOG_INFO, "KernelSU",
					    "Skipping managed feature '%.*s' (controlled by "
					    "module: %.*s)",
					    (int)managed[i].feature_length, managed[i].feature,
					    (int)managed[i].module_length, managed[i].module);
					break;
				}
		}
		if (config.count) {
			apply_config(&config);
			result = save_config(directory, path, &config);
			if (!result)
				__android_log_print(ANDROID_LOG_INFO, "KernelSU",
						    "Saved feature configuration to file");
		} else
			__android_log_print(ANDROID_LOG_INFO, "KernelSU",
					    "No features to apply, skipping initialization");
	}
done:
	int error = errno;
	free(config.entries);
	free(path);
	errno = error;
	return result;
}
