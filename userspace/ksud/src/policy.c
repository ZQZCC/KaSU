// SPDX-License-Identifier: GPL-3.0-or-later
#include "policy.h"
#include "driver.h"
#include "native.h"
#include "uapi/supercall.h"

#include <android/log.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int open_policy_fd(void)
{
	int driver = ksu_driver_fd();
	if (driver < 0) {
		errno = EBADF;
		return -1;
	}
	return ioctl(driver, KSU_IOCTL_GET_POLICY_FD, 0);
}

static int policy_metadata(int fd, struct ksu_policy_snapshot_cmd *info)
{
	*info = (struct ksu_policy_snapshot_cmd){0};
	return ioctl(fd, KSU_IOCTL_POLICY_SNAPSHOT, info);
}

static int restore_policy(int fd, const char *path)
{
	struct ksu_policy_snapshot_cmd info;
	if (policy_metadata(fd, &info))
		return -1;
	if (info.flags & KSU_POLICY_INITIALIZED)
		return 0;
	struct ksu_saved_policy stored;
	if (ksu_policy_load(path, &stored))
		return -1;
	struct ksu_policy_restore_cmd command = {
	    .profiles = (uintptr_t)stored.profiles,
	    .count = stored.length / KSU_POLICY_PROFILE_SIZE,
	    .flags = stored.needs_rewrite ? 0 : KSU_POLICY_RESTORE_SAVED,
	};
	int result = ioctl(fd, KSU_IOCTL_POLICY_RESTORE, &command);
	int error = errno;
	ksu_policy_free(&stored);
	errno = error;
	return result;
}

int ksu_policy_restore(const char *path)
{
	int fd = open_policy_fd();
	if (fd < 0)
		return errno == ENOTTY || errno == EOPNOTSUPP || errno == EALREADY ? 0 : -1;
	int result = restore_policy(fd, path);
	int error = errno;
	close(fd);
	errno = error;
	return result;
}

static int save_snapshot(int fd, const char *path)
{
	struct ksu_policy_snapshot_cmd info;
	if (policy_metadata(fd, &info))
		return -1;
	if (!(info.flags & KSU_POLICY_INITIALIZED)) {
		errno = EINVAL;
		return -1;
	}
	for (unsigned attempt = 0; attempt < 8; attempt++) {
		if (info.count > KSU_POLICY_MAX_PROFILES) {
			errno = EOVERFLOW;
			return -1;
		}
		unsigned char *profiles =
		    malloc(info.count ? info.count * KSU_POLICY_PROFILE_SIZE : 1);
		if (!profiles)
			return -1;
		struct ksu_policy_snapshot_cmd command = {
		    .profiles = (uintptr_t)profiles,
		    .count = info.count,
		};
		if (ioctl(fd, KSU_IOCTL_POLICY_SNAPSHOT, &command)) {
			int error = errno;
			free(profiles);
			errno = error;
			if (error != ENOSPC)
				return -1;
			info = command;
			continue;
		}
		int result;
		if (command.count > info.count) {
			errno = EOVERFLOW;
			result = -1;
		} else
			result = ksu_policy_save(path, profiles,
						 command.count * KSU_POLICY_PROFILE_SIZE);
		int error = errno;
		free(profiles);
		errno = error;
		if (result)
			return -1;
		return ioctl(fd, KSU_IOCTL_POLICY_ACK, &command.generation);
	}
	errno = EAGAIN;
	return -1;
}

int ksu_policy_daemon(const char *path)
{
	int fd = open_policy_fd();
	if (fd < 0)
		return -1;
	if (restore_policy(fd, path))
		goto done;
	__android_log_print(ANDROID_LOG_INFO, "KernelSU", "policy storage ready");
	for (;;) {
		struct pollfd event = {.fd = fd, .events = POLLIN};
		if (poll(&event, 1, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (event.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			errno = EPIPE;
			break;
		}
		if ((event.revents & POLLIN) && save_snapshot(fd, path))
			break;
	}
done:
	int error = errno;
	close(fd);
	errno = error;
	return -1;
}
