// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "assets.h"
#include "boot.h"
#include "process.h"
#include "prop.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/memfd.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOG(priority, ...) __android_log_print(priority, "KernelSU", __VA_ARGS__)

struct waitsys {
	pid_t pid;
	int fd;
};

static int start_waitsys(struct waitsys *child)
{
	int executable = syscall(SYS_memfd_create, "waitsys", MFD_CLOEXEC);
	if (executable < 0)
		return -1;
	int pipe_fd[] = {-1, -1}, result = -1;
	char **environment = NULL;
	if (ksu_asset_write("waitsys", executable) || pipe2(pipe_fd, O_CLOEXEC) ||
	    fcntl(pipe_fd[1], F_SETFD, 0))
		goto done;
	char path[64], output[48];
	snprintf(path, sizeof(path), "/proc/self/fd/%d", executable);
	snprintf(output, sizeof(output), "KSU_WAITSYS_FD=%d", pipe_fd[1]);
	size_t count = 0;
	while (environ[count])
		++count;
	environment = malloc((count + 2) * sizeof(*environment));
	if (!environment)
		goto done;
	size_t length = 0;
	for (size_t i = 0; i < count; ++i)
		if (strncmp(environ[i], "KSU_WAITSYS_FD=", sizeof("KSU_WAITSYS_FD=") - 1))
			environment[length++] = environ[i];
	environment[length++] = output;
	environment[length] = NULL;
	char *arguments[] = {path, NULL};
	pid_t pid = ksu_spawn(arguments, environment, -1, false);
	if (pid < 0)
		goto done;
	child->pid = pid;
	child->fd = pipe_fd[0];
	pipe_fd[0] = -1;
	result = 0;
done:;
	int error = errno;
	free(environment);
	close(executable);
	for (size_t i = 0; i < 2; ++i)
		if (pipe_fd[i] >= 0)
			close(pipe_fd[i]);
	errno = error;
	return result;
}

static int wait_signal(int fd, unsigned char expected, int64_t timeout)
{
	int64_t now = ksu_now_ns();
	if (now < 0)
		return -1;
	int64_t deadline = now + timeout;
	for (;;) {
		now = ksu_now_ns();
		if (now < 0)
			return -1;
		if (now >= deadline) {
			errno = ETIMEDOUT;
			return -1;
		}
		int64_t remaining = deadline - now;
		struct timespec duration = {remaining / 1000000000, remaining % 1000000000};
		struct pollfd descriptor = {.fd = fd, .events = POLLIN};
		int ready = ppoll(&descriptor, 1, &duration, NULL);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready <= 0) {
			if (!ready)
				errno = ETIMEDOUT;
			return -1;
		}
		unsigned char signal;
		ssize_t length = read(fd, &signal, 1);
		if (length < 0 && errno == EINTR)
			continue;
		if (length != 1 || signal != expected) {
			if (length >= 0)
				errno = length ? EPROTO : EPIPE;
			return -1;
		}
		return 0;
	}
}

static void stop_waitsys(struct waitsys *child)
{
	if (child->pid > 0) {
		pid_t pid = child->pid;
		child->pid = -1;
		kill(pid, SIGKILL);
		int status;
		if (ksu_wait_child(pid, &status))
			LOG(ANDROID_LOG_WARN, "failed to clean up waitsys: %s", strerror(errno));
	}
	if (child->fd >= 0) {
		close(child->fd);
		child->fd = -1;
	}
}

static int service_control(char *name)
{
	LOG(ANDROID_LOG_INFO, "%s", name);
	char *arguments[] = {name, NULL};
	pid_t pid = ksu_spawn(arguments, NULL, -1, false);
	int status;
	if (pid < 0 || ksu_wait_child(pid, &status))
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		LOG(ANDROID_LOG_WARN, "%s exited with status: %d", name, status);
	return 0;
}

static int soft_reboot(void)
{
	LOG(ANDROID_LOG_INFO, "emulating soft_reboot!");
	bool rebuild;
	static const unsigned char name[] = "sys.boot_completed";
	if (ksu_prop_init() ||
	    ksu_prop_set(name, sizeof(name) - 1, (const unsigned char *)"0", 1, true, &rebuild))
		LOG(ANDROID_LOG_WARN, "reset boot completed failed: %s", strerror(errno));
	int64_t now = ksu_now_ns();
	if (now < 0)
		return -1;
	ksu_run_stage(KSU_STAGE_SOFT_REBOOT, now + INT64_C(5000000000));
	struct waitsys child = {.pid = -1, .fd = -1};
	if (start_waitsys(&child))
		LOG(ANDROID_LOG_WARN, "failed to start waitsys: %s", strerror(errno));
	if (child.pid > 0 && wait_signal(child.fd, 1, INT64_C(2000000000))) {
		LOG(ANDROID_LOG_WARN, "waitsys failed to collect services: %s", strerror(errno));
		stop_waitsys(&child);
	}
	int result = -1;
	if (service_control("stop"))
		goto done;
	if (child.pid > 0 && wait_signal(child.fd, 2, INT64_C(5000000000)))
		LOG(ANDROID_LOG_WARN, "waitsys failed while waiting for services to stop: %s",
		    strerror(errno));
	stop_waitsys(&child);
	LOG(ANDROID_LOG_INFO, "post-fs-data");
	if (ksu_post_fs_data())
		goto done;
	if (service_control("start"))
		goto done;
	LOG(ANDROID_LOG_INFO, "services");
	ksu_services();
	if (ksu_prop_init() ||
	    ksu_prop_wait(name, sizeof(name) - 1, (const unsigned char *)"0", 1, NULL))
		LOG(ANDROID_LOG_WARN, "wait for boot completed failed: %s", strerror(errno));
	ksu_boot_completed();
	result = 0;
done:;
	int error = errno;
	stop_waitsys(&child);
	errno = error;
	return result;
}

int ksu_soft_reboot(void)
{
	if (!ksu_uapi_matches())
		return 0;
	int child = ksu_create_daemon(true);
	if (child < 0)
		return -1;
	if (!child)
		_exit(0);
	if (soft_reboot())
		return -1;
	_exit(0);
}
