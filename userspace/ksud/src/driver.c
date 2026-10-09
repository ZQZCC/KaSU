// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "driver.h"
#include "uapi/supercall.h"

#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

static _Thread_local volatile sig_atomic_t svc_in_flight;
static _Thread_local volatile sig_atomic_t sigsys_occurred;
static pthread_once_t driver_once = PTHREAD_ONCE_INIT;
static int driver_fd = -1;
static pthread_once_t info_once = PTHREAD_ONCE_INIT;
static struct ksu_get_info_cmd driver_info;

static void sigsys_handler(int sig, siginfo_t *info, void *context)
{
	(void)sig;
	if (!info || !context || info->si_code != 1)
		return;
	if (svc_in_flight)
		sigsys_occurred = 1;
	((ucontext_t *)context)->uc_mcontext.regs[0] = (unsigned long)-EPERM;
}

void ksu_setup_sigsys(void)
{
	struct sigaction action = {.sa_sigaction = sigsys_handler, .sa_flags = SA_SIGINFO};
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGSYS, &action, NULL))
		__android_log_print(ANDROID_LOG_WARN, "KernelSU",
				    "Failed to set SIGSYS handler: %s", strerror(errno));
}

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

int ksu_claim_driver_fd(void)
{
	/* Called at the single-threaded su entry, before any ioctl. */
	return scan_driver_fd(&driver_fd);
}

static void init_driver_fd(void)
{
	if (driver_fd >= 0)
		return;
	(void)scan_driver_fd(&driver_fd);
	if (driver_fd >= 0)
		return;

	int control = open(KSU_TINYFS_CONTROL_PATH, O_RDONLY | O_CLOEXEC);
	if (control >= 0) {
		driver_fd = ioctl(control, KSU_IOCTL_TINYFS_GET_DRIVER_FD, 0);
		close(control);
		if (driver_fd >= 0)
			return;
	}
	driver_fd = -1;
	svc_in_flight = 1;
	syscall(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0, &driver_fd);
	svc_in_flight = 0;
	if (sigsys_occurred) {
		sigsys_occurred = 0;
		fputs("KernelSU driver install syscall was blocked by seccomp\n", stderr);
		__android_log_print(ANDROID_LOG_ERROR, "KernelSU",
				    "KernelSU driver install syscall was blocked by seccomp");
	}
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
