// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "driver.h"
#include "uapi/supercall.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static pthread_once_t driver_once = PTHREAD_ONCE_INIT;
static int driver_fd = -1;
static pthread_once_t info_once = PTHREAD_ONCE_INIT;
static struct ksu_get_info_cmd driver_info;

static int scan_driver_fd(int *found)
{
	DIR *dir = opendir("/proc/self/fd");
	struct dirent *entry;
	int fd = -1;
	if (!dir)
		return -1;
	while ((entry = readdir(dir))) {
		char *end;
		long number = strtol(entry->d_name, &end, 10);
		char path[64], target[64];
		if (!*entry->d_name || *end || number < 0 || number > INT32_MAX)
			continue;
		snprintf(path, sizeof(path), "/proc/self/fd/%ld", number);
		ssize_t length = readlink(path, target, sizeof(target) - 1);
		if (length < 0)
			continue;
		target[length] = '\0';
		if (!strcmp(target, "anon_inode:[ksu_driver_su]")) {
			fd = (int)number;
			break;
		}
		if (!strcmp(target, "anon_inode:[ksu_driver]"))
			fd = (int)number;
	}
	closedir(dir);
	*found = fd;
	return 0;
}

int ksu_claim_driver_fd(const char *inherited_fd)
{
	/* Called at the single-threaded su entry, before any ioctl. */
	if (!inherited_fd)
		return scan_driver_fd(&driver_fd);
	char *end;
	errno = 0;
	long fd = strtol(inherited_fd, &end, 10);
	if (*inherited_fd < '0' || *inherited_fd > '9' || *end || errno || fd > INT32_MAX) {
		errno = EINVAL;
		return -1;
	}
	if (fcntl((int)fd, F_GETFD) < 0)
		return -1;
	driver_fd = (int)fd;
	return 0;
}

static void init_driver_fd(void)
{
	if (driver_fd >= 0)
		return;
	(void)scan_driver_fd(&driver_fd);
	if (driver_fd >= 0)
		return;

	int control = open(KSU_TINYFS_CONTROL_PATH, O_RDONLY | O_CLOEXEC);
	if (control < 0)
		return;
	driver_fd = ioctl(control, KSU_IOCTL_TINYFS_GET_DRIVER_FD, 0);
	close(control);
}

int ksu_driver_fd(void)
{
	pthread_once(&driver_once, init_driver_fd);
	return driver_fd;
}

int ksu_ioctl(uint32_t request, void *arg)
{
	int fd = ksu_driver_fd();
	if (fd < 0) {
		errno = EBADF;
		return -1;
	}
	return ioctl(fd, request, arg);
}

static void init_driver_info(void)
{
	if (ksu_ioctl(KSU_IOCTL_GET_INFO, &driver_info) < 0)
		(void)ksu_ioctl(KSU_IOCTL_GET_INFO_LEGACY, &driver_info);
}

const struct ksu_get_info_cmd *ksu_get_driver_info(void)
{
	pthread_once(&info_once, init_driver_info);
	return &driver_info;
}
