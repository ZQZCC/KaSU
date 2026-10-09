// SPDX-License-Identifier: GPL-3.0-or-later
#include "cli.h"
#include "driver.h"
#include "file.h"
#include "process.h"
#include "text.h"
#include "uapi/ksu.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifndef KSU_DEBUG_MODULE_DIR
#define KSU_DEBUG_MODULE_DIR "/sys/module/kernelsu"
#endif
#ifndef KSU_DATA_DIR
#define KSU_DATA_DIR "/data/data"
#endif

static int read_appid(uint32_t *appid)
{
	size_t length;
	unsigned char *owned =
	    ksu_read_file(KSU_DEBUG_MODULE_DIR "/parameters/ksu_debug_manager_appid", &length);
	if (!owned)
		return -1;
	const unsigned char *text = owned;
	ksu_trim_space(&text, &length);
	if (length && *text == '+') {
		++text;
		--length;
	}
	uint32_t value = 0;
	int result = length ? 0 : -1;
	for (size_t i = 0; i < length; ++i) {
		unsigned digit = text[i] - '0';
		if (digit > 9 || value > (UINT32_MAX - digit) / 10) {
			result = -1;
			break;
		}
		value = value * 10 + digit;
	}
	if (!result)
		*appid = value;
	else
		errno = EINVAL;
	free(owned);
	return result;
}

int ksu_debug_manager(const char *package)
{
	struct stat st;
	if (stat(KSU_DEBUG_MODULE_DIR, &st)) {
		fputs("CONFIG_KSU_DEBUG is not enabled\n", stderr);
		errno = 0;
		return -1;
	}
	char *path = ksu_join_path(KSU_DATA_DIR, package);
	if (!path)
		return -1;
	int result = stat(path, &st);
	free(path);
	if (result)
		return -1;
	uint32_t before, after;
	if (read_appid(&before))
		return -1;
	char text[16];
	int length = snprintf(text, sizeof(text), "%u", (unsigned)st.st_uid % 100000);
	if (ksu_write_file(KSU_DEBUG_MODULE_DIR "/parameters/ksu_debug_manager_appid", text,
			   (size_t)length) ||
	    read_appid(&after))
		return -1;
	printf("set manager appid: %u -> %u\n", before, after);
	char *args[] = {"am", "force-stop", (char *)package, NULL};
	pid_t child = ksu_spawn(args, NULL, -1, false);
	if (child >= 0)
		(void)ksu_wait_child(child, NULL);
	return 0;
}

int ksu_debug_mark(unsigned operation, int32_t pid)
{
	struct ksu_manage_mark_cmd cmd = {.operation = operation, .pid = pid};
	if (ksu_ioctl(KSU_IOCTL_MANAGE_MARK, &cmd) < 0)
		return -1;
	switch (operation) {
	case KSU_MARK_GET:
		if (!pid) {
			fputs("Please specify a pid to get its mark status\n", stderr);
			errno = 0;
			return -1;
		}
		printf("Process %d mark status: %s\n", pid, cmd.result ? "marked" : "unmarked");
		break;
	case KSU_MARK_MARK:
	case KSU_MARK_UNMARK: {
		const char *word = operation == KSU_MARK_MARK ? "marked" : "unmarked";
		if (pid)
			printf("Process %d %s successfully\n", pid, word);
		else
			printf("All processes %s successfully\n", word);
		break;
	}
	case KSU_MARK_REFRESH:
		puts("Refreshed mark for all running processes");
		break;
	}
	return 0;
}
