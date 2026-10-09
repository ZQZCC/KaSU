// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "assets.h"
#include "boot.h"
#include "cleanup.h"
#include "file.h"
#include "native.h"
#include "paths.h"
#include "prop.h"

#include <android/log.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void refresh_rc(void)
{
	if (ksu_regenerate_boot_rc())
		__android_log_print(ANDROID_LOG_WARN, "KernelSU",
				    "regenerate preinit rc failed: %s", strerror(errno));
}

static int install(const char *zip, char **message)
{
	if (!ksu_property_equals("sys.boot_completed", "1")) {
		*message = strdup("Android is Booting!");
		errno = EBUSY;
		return -1;
	}
	printf("%s\n", ksu_banner);
	if (ksu_assets_ensure(KSU_BINARY_DIR, KSU_DAEMON_PATH, false) ||
	    ksu_mkdirs(KSU_WORKING_DIR) || ksu_mkdirs(KSU_BINARY_DIR))
		return -1;
	KSU_AUTO_FREE char *path = realpath(zip, NULL);
	if (!path)
		return -1;
	return ksu_install_module(path, KSU_BUSYBOX_PATH, KSU_MODULE_DIR "/",
				  KSU_MODULE_UPDATE_DIR "/", KSU_METAMODULE_DIR, ksu_installer,
				  message);
}

int ksu_module_run(enum ksu_module_command command, const char *value, char **message)
{
	*message = NULL;
	if (command == KSU_MODULE_CMD_LIST)
		return ksu_list_modules(KSU_MODULE_DIR, KSU_MODULE_CONFIG_DIR);
	if (command != KSU_MODULE_CMD_INSTALL && command != KSU_MODULE_CMD_DISABLE &&
	    !ksu_valid_identifier((const unsigned char *)value, strlen(value))) {
		asprintf(message, "Invalid module ID: '%s'. Must match /^[a-zA-Z][a-zA-Z0-9._-]+$/",
			 value);
		errno = EINVAL;
		return -1;
	}
	if ((command == KSU_MODULE_CMD_INSTALL || command == KSU_MODULE_CMD_ACTION) &&
	    !ksu_uapi_matches()) {
		errno = EPROTO;
		return -1;
	}
	if (command == KSU_MODULE_CMD_INSTALL) {
		int result = install(value, message);
		if (result < 0)
			printf("- Error: %s\n", *message ? *message : strerror(errno));
		else
			refresh_rc();
		return result;
	}
	KSU_AUTO_FREE char *directory = ksu_join_path(KSU_MODULE_DIR, value);
	if (!directory)
		return -1;
	int result;
	if (command == KSU_MODULE_CMD_ACTION) {
		KSU_AUTO_FREE char *path = ksu_join_path(directory, "action.sh");
		result =
		    path ? ksu_exec_script(KSU_BUSYBOX_PATH, path, directory, value, NULL, -1) : -1;
	} else {
		enum ksu_module_state state;
		const char *action;
		switch (command) {
		case KSU_MODULE_CMD_ENABLE:
			state = KSU_MODULE_ENABLE;
			action = "enabled";
			break;
		case KSU_MODULE_CMD_DISABLE:
			state = KSU_MODULE_DISABLE;
			action = "disabled";
			break;
		case KSU_MODULE_CMD_REMOVE:
			state = KSU_MODULE_REMOVE;
			action = "marked for removal";
			break;
		default:
			state = KSU_MODULE_UNDO_REMOVE;
			action = "remove mark cleared";
		}
		result = ksu_module_mark(directory, state);
		if (result >= 0) {
			if (result)
				__android_log_print(ANDROID_LOG_INFO, "KernelSU", "Module %s %s",
						    value, action);
			refresh_rc();
			result = 0;
		}
	}
	return result;
}

int ksu_install_userspace(void)
{
	if (ksu_mkdirs(KSU_ADB_DIR))
		return -1;
	unlink(KSU_DAEMON_PATH);
	if (ksu_copy_file("/proc/self/exe", KSU_DAEMON_PATH) ||
	    ksu_setfilecon(KSU_DAEMON_PATH, "u:object_r:ksu_file:s0") ||
	    ksu_assets_ensure(KSU_BINARY_DIR, KSU_DAEMON_PATH, false))
		return -1;
	struct stat st;
	if (!stat(KSU_DAEMON_PATH, &st) && stat(KSU_BINARY_DIR "/ksud", &st))
		return symlink(KSU_DAEMON_PATH, KSU_BINARY_DIR "/ksud");
	return 0;
}
