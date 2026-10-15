// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "boot.h"
#include "cleanup.h"
#include "file.h"
#include "native.h"
#include "paths.h"
#include "process.h"
#include "text.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Missing scripts are optional; existing ones must stay inside the module. */
static int check_script(const char *staged, const char *script)
{
	KSU_AUTO_FREE char *path = NULL, *resolved = NULL, *root = NULL;
	if (!*script)
		return 0;
	if (*script == '/')
		goto invalid;
	for (const char *part = script; *part;) {
		size_t length = strcspn(part, "/");
		if (length == 2 && !memcmp(part, "..", 2))
			goto invalid;
		part += length;
		if (*part)
			++part;
	}
	path = ksu_join_path(staged, script);
	if (!path)
		return -1;
	struct stat st;
	if (lstat(path, &st)) {
		if (errno != ENOENT)
			return -1;
		printf("! MODULE_HOT_RUN_SCRIPT '%s' not found, skipping it\n", script);
		return 0;
	}
	resolved = realpath(path, NULL);
	root = realpath(staged, NULL);
	if (!resolved || !root || stat(resolved, &st))
		return -1;
	size_t length = strlen(root);
	if (S_ISREG(st.st_mode) && !strncmp(resolved, root, length) && resolved[length] == '/')
		return 1;
invalid:
	errno = EINVAL;
	return -1;
}

int ksu_hot_install(const char *shell, const char *staged, const char *live, const char *id,
		    int is_metamodule)
{
	KSU_AUTO_FREE char *marker = ksu_join_path(staged, KSU_HOT_INSTALL_MARKER);
	if (!marker)
		return -1;
	size_t length;
	KSU_AUTO_FREE unsigned char *data = ksu_read_file(marker, &length);
	if (!data)
		return errno == ENOENT ? 0 : -1;
	if (unlink(marker))
		return -1;
	puts("- Hot install requested");
	if (is_metamodule || ksu_is_safe_mode()) {
		puts("- Hot install skipped: metamodule or safe mode; reboot to apply");
		return 0;
	}
	if (!ksu_valid_utf8(data, length) || memchr(data, 0, length)) {
		puts("- Hot install skipped: invalid request; reboot to apply");
		return 0;
	}
	const unsigned char *text = data;
	ksu_trim_space(&text, &length);
	KSU_AUTO_FREE char *script = strndup((const char *)text, length);
	if (!script)
		return -1;
	int run = check_script(staged, script);
	if (run < 0) {
		printf("- Hot install skipped: invalid MODULE_HOT_RUN_SCRIPT '%s': %s\n", script,
		       strerror(errno));
		return 0;
	}
	KSU_AUTO_FREE char *rule = ksu_join_path(live, "sepolicy.rule");
	KSU_AUTO_FREE char *executable = run ? ksu_join_path(live, script) : NULL;
	KSU_AUTO_FREE char *backup = NULL;
	if (!rule || (run && !executable))
		return -1;
	struct stat st;
	if (!lstat(live, &st)) {
		if (ksu_mkdirs(KSU_HOT_INSTALL_TRASH_DIR) ||
		    asprintf(&backup, KSU_HOT_INSTALL_TRASH_DIR "/%s.XXXXXX", id) < 0 ||
		    !mkdtemp(backup))
			goto skipped;
		if (rename(live, backup)) {
			int error = errno;
			rmdir(backup);
			errno = error;
			goto skipped;
		}
	} else if (errno != ENOENT) {
		goto skipped;
	}
	if (rename(staged, live)) {
		int error = errno;
		if (backup && rename(backup, live))
			return -1;
		errno = error;
		goto skipped;
	}
	if (!stat(rule, &st)) {
		KSU_AUTO_FREE char *message = NULL;
		if (ksu_sepolicy_file(rule, 0, &message))
			printf("! Failed to apply sepolicy.rule: %s\n",
			       message ? message : strerror(errno));
	} else if (errno != ENOENT) {
		printf("! Failed to read sepolicy.rule: %s\n", strerror(errno));
	}
	if (run) {
		printf("- Running %s\n", script);
		/* This install command exits next; its child keeps logs but not installer stdin. */
		if (signal(SIGPIPE, SIG_IGN) == SIG_ERR || !freopen("/dev/null", "r", stdin))
			printf("! Failed to configure hot install script: %s\n", strerror(errno));
		else {
			int result = ksu_exec_script(shell, executable, live, id, NULL,
						     INT64_C(30000000000));
			if (result)
				printf("! Hot install script: %s\n",
				       result < 0 ? strerror(errno) : "timed out");
		}
	}
	return 1;
skipped:
	printf("- Hot install skipped: %s; reboot to apply\n", strerror(errno));
	return 0;
}

int ksu_clean_hot_install_trash(void)
{
	const char *old = KSU_HOT_INSTALL_TRASH_DIR ".old";
	if (ksu_remove_tree(old) && errno != ENOENT)
		return -1;
	if (rename(KSU_HOT_INSTALL_TRASH_DIR, old))
		return errno == ENOENT ? 0 : -1;
	char *arguments[] = {KSU_BUSYBOX_PATH, "rm", "-rf", (char *)old, NULL};
	return ksu_spawn(arguments, NULL, -1, true) < 0 ? ksu_remove_tree(old) : 0;
}
