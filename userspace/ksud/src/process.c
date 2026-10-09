// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "process.h"
#include "driver.h"
#include "uapi/supercall.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int64_t ksu_now_ns(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now))
		return -1;
	return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

int ksu_switch_mnt_ns(int pid)
{
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/ns/mnt", pid);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	char *cwd = getcwd(NULL, 0);
	int result = setns(fd, CLONE_NEWNS);
	int error = errno;
	close(fd);
	if (!result && cwd)
		(void)chdir(cwd);
	free(cwd);
	errno = error;
	return result;
}

static void switch_cgroup(const char *path)
{
	int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
	if (fd < 0)
		return;
	char pid[32];
	int length = snprintf(pid, sizeof(pid), "%d", getpid());
	(void)write(fd, pid, (size_t)length);
	close(fd);
}

void ksu_switch_cgroups(void)
{
	switch_cgroup("/acct/cgroup.procs");
	switch_cgroup("/dev/cg2_bpf/cgroup.procs");
	switch_cgroup("/sys/fs/cgroup/cgroup.procs");
	char value[PROP_VALUE_MAX] = "";
	__system_property_get("ro.config.per_app_memcg", value);
	if (strcmp(value, "false"))
		switch_cgroup("/dev/memcg/apps/cgroup.procs");
}

void ksu_detach_process_group(int use_init_pgrp)
{
	if (use_init_pgrp) {
		if (ksu_ioctl(KSU_IOCTL_SET_INIT_PGRP, NULL) >= 0)
			return;
		__android_log_print(ANDROID_LOG_ERROR, "KernelSU",
				    "failed to switch to init group: %s", strerror(errno));
	}
	if (setpgid(0, 0))
		__android_log_print(ANDROID_LOG_ERROR, "KernelSU",
				    "failed to set process group: %s", strerror(errno));
}

int ksu_wait_child(pid_t pid, int *status)
{
	pid_t result;
	do {
		result = waitpid(pid, status, 0);
	} while (result < 0 && errno == EINTR);
	return result < 0 ? -1 : 0;
}

int ksu_create_daemon(bool init_namespace)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid > 0) {
		int status;
		if (ksu_wait_child(pid, &status))
			_exit(1);
		if (WIFEXITED(status) && !WEXITSTATUS(status))
			return 0;
		errno = ECHILD;
		return -1;
	}
	ksu_detach_process_group(1);
	ksu_switch_cgroups();
	if (init_namespace && (ksu_switch_mnt_ns(1) || chdir("/")))
		goto failed;
	int fd = open("/dev/null", O_RDWR);
	if (fd < 0)
		goto failed;
	int result = 0;
	for (int standard = 0; standard < 3 && !result; ++standard)
		result = dup2(fd, standard) < 0 ? -1 : 0;
	int error = errno;
	if (fd > STDERR_FILENO)
		close(fd);
	errno = error;
	if (result)
		goto failed;
	pid = fork();
	if (pid < 0)
		goto failed;
	if (pid > 0)
		_exit(0);
	return 1;
failed:
	__android_log_print(ANDROID_LOG_ERROR, "KernelSU", "failed to configure daemon: %s",
			    strerror(errno));
	_exit(1);
}

pid_t ksu_spawn(char *const arguments[], char *const environment[], int output, bool detached)
{
	int pipe_fd[2];
	if (pipe2(pipe_fd, O_CLOEXEC))
		return -1;
	pid_t pid = fork();
	if (!pid) {
		close(pipe_fd[0]);
		if (detached) {
			if (setpgid(0, 0))
				goto failed;
			ksu_switch_cgroups();
		}
		if (output >= 0) {
			if (output == STDOUT_FILENO) {
				if (fcntl(output, F_SETFD, 0))
					goto failed;
			} else if (dup2(output, STDOUT_FILENO) < 0)
				goto failed;
		}
		if (environment)
			execvpe(arguments[0], arguments, environment);
		else
			execvp(arguments[0], arguments);
	failed:;
		int error = errno;
		while (write(pipe_fd[1], &error, sizeof(error)) < 0 && errno == EINTR)
			;
		_exit(127);
	}
	int error = errno;
	close(pipe_fd[1]);
	if (pid > 0) {
		int child_error;
		ssize_t count;
		do {
			count = read(pipe_fd[0], &child_error, sizeof(child_error));
		} while (count < 0 && errno == EINTR);
		if (count != 0) {
			error = count == sizeof(child_error) ? child_error
				: count < 0		     ? errno
							     : EIO;
			int status;
			ksu_wait_child(pid, &status);
			pid = -1;
		}
	}
	close(pipe_fd[0]);
	errno = error;
	return pid;
}
