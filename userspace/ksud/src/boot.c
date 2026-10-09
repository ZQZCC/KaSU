// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "boot.h"
#include "assets.h"
#include "driver.h"
#include "file.h"
#include "native.h"
#include "paths.h"
#include "process.h"
#include "prop.h"
#include "uapi/ksu.h"

#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOG(priority, ...) __android_log_print(priority, "KernelSU", __VA_ARGS__)

static const struct {
	const char *name, *common;
} stages[] = {
    {"post-fs-data", KSU_ADB_DIR "/post-fs-data.d"},
    {"post-mount", KSU_ADB_DIR "/post-mount.d"},
    {"service", KSU_ADB_DIR "/service.d"},
    {"boot-completed", KSU_ADB_DIR "/boot-completed.d"},
    {"emulated-soft-reboot", KSU_ADB_DIR "/emulated-soft-reboot.d"},
};

static void warn_failed(int result, const char *action)
{
	if (result < 0)
		LOG(ANDROID_LOG_WARN, "%s failed: %s", action, strerror(errno));
}

bool ksu_uapi_matches(void)
{
	uint32_t version = ksu_get_driver_info()->uapi_version;
	if (version == KERNEL_SU_UAPI_VERSION)
		return true;
	LOG(ANDROID_LOG_ERROR, "UAPI version mismatch: kernel=%u, ksud=%u. Please update KernelSU!",
	    version, KERNEL_SU_UAPI_VERSION);
	return false;
}

bool ksu_is_safe_mode(void)
{
	bool safe = ksu_property_equals("persist.sys.safemode", "1") ||
		    ksu_property_equals("ro.sys.safemode", "1");
	LOG(ANDROID_LOG_INFO, "safemode: %s", safe ? "true" : "false");
	if (safe)
		return true;
	struct ksu_check_safemode_cmd command = {0};
	ksu_ioctl(KSU_IOCTL_CHECK_SAFEMODE, &command);
	safe = command.in_safe_mode != 0;
	LOG(ANDROID_LOG_INFO, "kernel_safemode: %s", safe ? "true" : "false");
	return safe;
}

static bool has_magisk(void)
{
	const char *paths = getenv("PATH");
	if (!paths)
		return false;
	for (const char *start = paths;;) {
		const char *end = strchr(start, ':');
		size_t length = end ? (size_t)(end - start) : strlen(start);
		char *path;
		if (asprintf(&path, "%.*s%smagisk", (int)length, start,
			     length && start[length - 1] != '/' ? "/" : "") < 0)
			return false;
		struct stat st;
		bool found = !stat(path, &st) && S_ISREG(st.st_mode) && !access(path, X_OK);
		free(path);
		if (found || !end)
			return found;
		start = end + 1;
	}
}

static int remaining(int64_t deadline, int64_t *timeout)
{
	*timeout = deadline;
	if (deadline <= 0)
		return 0;
	int64_t now = ksu_now_ns();
	if (now < 0)
		return -1;
	*timeout = deadline > now ? deadline - now : 0;
	return 0;
}

static int catch_bootlog(const char *name, const char *command, const char *arg1, const char *arg2)
{
	if (ksu_mkdirs(KSU_LOG_DIR))
		return -1;
	char *path = NULL, *old = NULL;
	int result = -1, fd = -1;
	if (asprintf(&path, "%s/%s.log", KSU_LOG_DIR, name) < 0 ||
	    asprintf(&old, "%s/%s.old.log", KSU_LOG_DIR, name) < 0)
		goto done;
	struct stat st;
	if (!stat(path, &st) && rename(path, old))
		goto done;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
	if (fd < 0)
		goto done;
	char *args[] = {"timeout",	 "-s",	       "9",	     "30s",
			(char *)command, (char *)arg1, (char *)arg2, NULL};
	if (ksu_spawn(args, NULL, fd, true) < 0)
		LOG(ANDROID_LOG_WARN, "Failed to start logcat: %s", strerror(errno));
	result = 0;
done:;
	int error = errno;
	if (fd >= 0)
		close(fd);
	free(path);
	free(old);
	errno = error;
	return result;
}

int ksu_regenerate_boot_rc(void)
{
	struct stat st;
	bool watchdog = !stat(KSU_METADATA_DIR "/watchdog", &st) && S_ISDIR(st.st_mode);
	const char *directory = watchdog ? KSU_PREINIT_WATCHDOG : KSU_PREINIT_DEFAULT;
	if (ksu_regenerate_rc(KSU_ADB_DIR "/initrc.d", KSU_MODULE_DIR, KSU_MODULE_UPDATE_DIR,
			      directory))
		return -1;
	const char *output =
	    watchdog ? KSU_PREINIT_WATCHDOG "/modules.rc" : KSU_PREINIT_DEFAULT "/modules.rc";
	if (ksu_setfilecon(output, "u:object_r:metadata_file:s0"))
		LOG(ANDROID_LOG_DEBUG, "set context on %s failed: %s", output, strerror(errno));
	unlink(watchdog ? KSU_PREINIT_DEFAULT "/modules.rc" : KSU_PREINIT_WATCHDOG "/modules.rc");
	return 0;
}

int ksu_disable_modules(void)
{
	DIR *directory = opendir(KSU_MODULE_DIR);
	if (!directory)
		return -1;
	struct dirent *entry;
	while ((entry = readdir(directory))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		char *path;
		if (asprintf(&path, "%s/%s/disable", KSU_MODULE_DIR, entry->d_name) < 0)
			continue;
		if (ksu_ensure_file(path))
			LOG(ANDROID_LOG_WARN, "Failed to mark module: %s: %s", path,
			    strerror(errno));
		free(path);
	}
	closedir(directory);
	warn_failed(ksu_regenerate_boot_rc(), "regenerate preinit rc");
	return 0;
}

static int load_module_files(bool properties)
{
	DIR *directory = opendir(KSU_MODULE_DIR);
	if (!directory)
		return -1;
	bool property_ready = false;
	int result = 0;
	struct dirent *entry;
	while ((entry = readdir(directory))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		char *module = ksu_join_path(KSU_MODULE_DIR, entry->d_name), *path = NULL;
		int active = module ? ksu_module_active(module) : -1;
		if (active < 0) {
			result = -1;
			goto next;
		}
		if (!active)
			goto next;
		path = ksu_join_path(module, properties ? "system.prop" : "sepolicy.rule");
		if (!path) {
			result = -1;
			goto next;
		}
		struct stat st;
		if (stat(path, &st))
			goto next;
		LOG(ANDROID_LOG_INFO, "load %s", path);
		if (properties) {
			bool rebuild;
			if (!property_ready) {
				result = ksu_prop_init();
				if (result)
					goto next;
				property_ready = true;
			}
			result = ksu_prop_load_file(path, KSU_PROP_SKIP_SVC, &rebuild);
			if (!result && rebuild)
				LOG(ANDROID_LOG_WARN, "resetprop: warning: rebuild is needed!");
		} else {
			char *message = NULL;
			if (ksu_sepolicy_file(path, 0, &message))
				LOG(ANDROID_LOG_WARN, "Failed to load sepolicy.rule for %s%s%s",
				    path, message ? ": " : "", message ? message : "");
			free(message);
		}
	next:
		free(path);
		free(module);
		if (result)
			break;
	}
	int error = errno;
	closedir(directory);
	errno = error;
	return result;
}

static int apply_profiles(void)
{
	struct stat st;
	if (stat(KSU_PROFILE_SELINUX_DIR, &st)) {
		LOG(ANDROID_LOG_INFO, "profile sepolicy dir not exists.");
		return 0;
	}
	DIR *directory = opendir(KSU_PROFILE_SELINUX_DIR);
	if (!directory)
		return -1;
	struct dirent *entry;
	while ((entry = readdir(directory))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		char *path = ksu_join_path(KSU_PROFILE_SELINUX_DIR, entry->d_name);
		if (!path)
			continue;
		char *message = NULL;
		int result = ksu_sepolicy_file(path, 0, &message);
		LOG(ANDROID_LOG_INFO, "profile sepolicy %s: %s",
		    result ? "apply failed" : "applied", path);
		free(message);
		free(path);
	}
	closedir(directory);
	return 0;
}

static const char *component(const char **cursor, size_t *length)
{
	for (;;) {
		while (**cursor == '/')
			++*cursor;
		const char *start = *cursor;
		*length = strcspn(start, "/");
		*cursor += *length;
		if (*length != 1 || *start != '.')
			return *length ? start : NULL;
	}
}

static char *stage_module_id(const char *directory)
{
	const char *prefix = KSU_MODULE_DIR, *expected, *actual;
	size_t expected_length, length;
	if ((*prefix == '/') != (*directory == '/'))
		return NULL;
	while ((expected = component(&prefix, &expected_length))) {
		actual = component(&directory, &length);
		if (!actual || length != expected_length || memcmp(actual, expected, length))
			return NULL;
	}
	actual = component(&directory, &length);
	return actual && ksu_valid_identifier((const unsigned char *)actual, length)
		   ? strndup(actual, length)
		   : NULL;
}

static int meta_stage(const char *stage, int64_t deadline)
{
	char *directory = ksu_find_metamodule(KSU_METAMODULE_DIR, KSU_MODULE_DIR);
	if (!directory)
		return 0;
	char *path = NULL, *disabled = ksu_join_path(directory, "disable"), *id = NULL;
	int result = -1;
	if (!disabled || asprintf(&path, "%s/%s.sh", directory, stage) < 0)
		goto done;
	struct stat st;
	result = 0;
	if (!stat(disabled, &st)) {
		LOG(ANDROID_LOG_INFO, "Metamodule is disabled, skipping %s.sh", stage);
		goto done;
	}
	if (stat(path, &st))
		goto done;
	id = stage_module_id(directory);
	int64_t timeout;
	result = remaining(deadline, &timeout);
	if (!result) {
		LOG(ANDROID_LOG_INFO, "Executing metamodule %s.sh", stage);
		result = ksu_exec_script(KSU_BUSYBOX_PATH, path, directory, id, NULL, timeout);
		if (result > 0)
			LOG(ANDROID_LOG_WARN, "Timed out waiting for script: %s", path);
		if (result >= 0) {
			LOG(ANDROID_LOG_INFO, "Metamodule %s.sh executed successfully", stage);
			result = 0;
		}
	}
done:;
	int error = errno;
	free(path);
	free(disabled);
	free(id);
	free(directory);
	errno = error;
	return result;
}

static void module_stage(const char *stage, int64_t deadline)
{
	int64_t timeout;
	int result = remaining(deadline, &timeout);
	if (!result)
		result = ksu_exec_module_stage(KSU_BUSYBOX_PATH, KSU_MODULE_DIR, KSU_METAMODULE_DIR,
					       stage, timeout);
	warn_failed(result, "exec module stage scripts");
}

static void common_stage(enum ksu_boot_stage stage, int64_t deadline)
{
	int64_t timeout;
	int result = remaining(deadline, &timeout);
	if (!result)
		result = ksu_exec_common_scripts(KSU_BUSYBOX_PATH, stages[stage].common, timeout);
	warn_failed(result, "exec common stage scripts");
}

void ksu_run_stage(enum ksu_boot_stage stage, int64_t deadline)
{
	const char *name = stages[stage].name;
	umask(0);
	if (has_magisk()) {
		LOG(ANDROID_LOG_WARN, "Magisk detected, skip %s", name);
		return;
	}
	if (ksu_is_safe_mode()) {
		LOG(ANDROID_LOG_WARN, "safe mode, skip %s scripts", name);
		return;
	}
	common_stage(stage, deadline);
	warn_failed(meta_stage(name, deadline), "exec metamodule stage script");
	module_stage(name, deadline);
}

int ksu_post_fs_data(void)
{
	if (!ksu_uapi_matches())
		return 0;
	if (ksu_policy_restore(KSU_POLICY_PATH))
		return -1;
	struct ksu_report_event_cmd event = {.event = EVENT_POST_FS_DATA};
	ksu_ioctl(KSU_IOCTL_REPORT_EVENT, &event);
	umask(0);
	warn_failed(ksu_clear_temporary_configs(KSU_MODULE_CONFIG_DIR), "clear temp configs");
	catch_bootlog("logcat", "logcat", "-b", "all");
	catch_bootlog("dmesg", "dmesg", "-w", "-r");
	if (has_magisk()) {
		LOG(ANDROID_LOG_WARN, "Magisk detected, skip post-fs-data!");
		return 0;
	}
	bool safe = ksu_is_safe_mode();
	int64_t deadline = ksu_now_ns();
	if (deadline < 0)
		return -1;
	deadline += INT64_C(35000000000);
	if (safe)
		LOG(ANDROID_LOG_WARN, "safe mode, skip common post-fs-data.d scripts");
	else
		common_stage(KSU_STAGE_POST_FS_DATA, deadline);
	if (ksu_assets_ensure(KSU_BINARY_DIR, KSU_DAEMON_PATH, true))
		return -1;
	if (safe) {
		LOG(ANDROID_LOG_WARN,
		    "safe mode, skip post-fs-data scripts and disable all modules!");
		warn_failed(ksu_disable_modules(), "disable all modules");
		return 0;
	}
	warn_failed(ksu_apply_module_updates(KSU_MODULE_UPDATE_DIR, KSU_MODULE_DIR),
		    "handle updated modules");
	warn_failed(ksu_prune_modules(KSU_BUSYBOX_PATH, KSU_MODULE_DIR, KSU_METAMODULE_DIR,
				      KSU_MODULE_CONFIG_DIR),
		    "prune modules");
	warn_failed(ksu_regenerate_boot_rc(), "regenerate preinit rc");
	int result = ksu_setfilecon(KSU_DAEMON_PATH, "u:object_r:ksu_file:s0");
	if (!result)
		result = ksu_restore_syscon(KSU_MODULE_DIR, 1);
	warn_failed(result, "restorecon");
	warn_failed(load_module_files(false), "load sepolicy.rule");
	warn_failed(apply_profiles(), "apply root profile sepolicy");
	if (ksu_is_safe_mode())
		LOG(ANDROID_LOG_WARN, "safe mode, skip load feature config");
	else
		warn_failed(ksu_feature_modules_run(KSU_FEATURE_INIT, NULL, 0, KSU_WORKING_DIR,
						    KSU_MODULE_DIR, KSU_MODULE_CONFIG_DIR),
			    "init features");
	warn_failed(meta_stage("post-fs-data", deadline), "exec metamodule post-fs-data script");
	module_stage("post-fs-data", deadline);
	warn_failed(load_module_files(true), "load system.prop");
	int status;
	result = ksu_exec_metamodule(KSU_META_MOUNT, KSU_BUSYBOX_PATH, KSU_METAMODULE_DIR,
				     KSU_MODULE_DIR, KSU_MODULE_DIR "/", &status);
	if (result < 0)
		warn_failed(result, "execute metamodule mount");
	else if (result > 0) {
		if (!WIFEXITED(status) || WEXITSTATUS(status))
			LOG(ANDROID_LOG_WARN, "Metamodule mount script failed with status: %d",
			    status);
		else
			LOG(ANDROID_LOG_INFO, "Metamodule mount script executed successfully");
	}
	ksu_run_stage(KSU_STAGE_POST_MOUNT, deadline);
	return chdir("/");
}

void ksu_services(void)
{
	if (!ksu_uapi_matches())
		return;
	struct ksu_report_event_cmd command = {.event = EVENT_SERVICES};
	int result = ksu_ioctl(KSU_IOCTL_REPORT_EVENT, &command);
	if (result != 1) {
		if (result < 0)
			LOG(ANDROID_LOG_ERROR, "Failed to report services: %s", strerror(errno));
		else
			LOG(ANDROID_LOG_INFO, "services already started, skipping");
		return;
	}
	LOG(ANDROID_LOG_INFO, "on_services triggered!");
	ksu_run_stage(KSU_STAGE_SERVICE, 0);
}

void ksu_boot_completed(void)
{
	if (!ksu_uapi_matches())
		return;
	struct ksu_report_event_cmd command = {.event = EVENT_BOOT_COMPLETED};
	ksu_ioctl(KSU_IOCTL_REPORT_EVENT, &command);
	LOG(ANDROID_LOG_INFO, "on_boot_completed triggered!");
	ksu_run_stage(KSU_STAGE_BOOT_COMPLETED, 0);
}
