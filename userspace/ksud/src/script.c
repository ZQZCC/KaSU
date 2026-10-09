// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include "driver.h"
#include "file.h"
#include "native.h"
#include "process.h"
#include "text.h"
#include "uapi/ksu.h"

#include <android/log.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STRINGIFY_(value) #value
#define STRINGIFY(value) STRINGIFY_(value)

struct script_environment {
	char **values;
	size_t count;
	char version[48], uapi[32];
	char *path, *module;
};

static void free_environment(struct script_environment *environment)
{
	int error = errno;
	free(environment->values);
	free(environment->path);
	free(environment->module);
	errno = error;
}

static int compare_environment(const void *left, const void *right)
{
	const char *a = *(char *const *)left, *b = *(char *const *)right;
	size_t a_length = (size_t)(strchr(a, '=') - a);
	size_t b_length = (size_t)(strchr(b, '=') - b);
	int result = memcmp(a, b, a_length < b_length ? a_length : b_length);
	return result ? result : (a_length > b_length) - (a_length < b_length);
}

static void put_environment(struct script_environment *environment, char *value)
{
	if (!strchr(value, '='))
		return;
	for (size_t i = 0; i < environment->count; ++i) {
		if (!compare_environment(&environment->values[i], &value)) {
			environment->values[i] = value;
			return;
		}
	}
	environment->values[environment->count++] = value;
}

static int script_environment(struct script_environment *environment, const char *module_id,
			      char *const overrides[])
{
	size_t count = 10;
	for (char **value = environ; value && *value; ++value)
		++count;
	if (overrides)
		for (size_t i = 0; overrides[i]; ++i)
			++count;
	environment->values = calloc(count, sizeof(char *));
	if (!environment->values)
		return -1;
	const char *path = getenv("PATH");
	if (!path || !ksu_valid_utf8(path, strlen(path)))
		path = "";
	if (asprintf(&environment->path, "PATH=%s:/data/adb/ksu/bin", path) < 0)
		return -1;
	snprintf(environment->version, sizeof(environment->version), "KSU_KERNEL_VER_CODE=%d",
		 (int32_t)ksu_get_driver_info()->version);
	snprintf(environment->uapi, sizeof(environment->uapi), "KSU_UAPI_VER=%u",
		 KERNEL_SU_UAPI_VERSION);
	if (module_id) {
		if (ksu_valid_identifier((const unsigned char *)module_id, strlen(module_id))) {
			if (asprintf(&environment->module, "KSU_MODULE=%s", module_id) < 0)
				return -1;
		} else {
			__android_log_print(ANDROID_LOG_ERROR, "KernelSU",
					    "Invalid module_id provided: %s", module_id);
		}
	}
	char *common[] = {"ASH_STANDALONE=1",	       "KSU=true",
			  environment->version,	       "KSU_VER_CODE=" STRINGIFY(KSU_VERSION_CODE),
			  "KSU_VER=" KSU_VERSION_NAME, environment->uapi,
			  "KSU_RUNTIME_MODE=built-in", environment->path};
	for (char **value = environ; value && *value; ++value)
		put_environment(environment, *value);
	for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); ++i)
		put_environment(environment, common[i]);
	if (environment->module)
		put_environment(environment, environment->module);
	if (overrides)
		for (size_t i = 0; overrides[i]; ++i)
			put_environment(environment, overrides[i]);
	qsort(environment->values, environment->count, sizeof(char *), compare_environment);
	return 0;
}

static int wait_script(pid_t pid, const sigset_t *signals, int64_t deadline, int *status)
{
	for (;;) {
		pid_t result = waitpid(pid, status, deadline < 0 ? 0 : WNOHANG);
		if (result == pid)
			return 0;
		if (result < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		int64_t now = ksu_now_ns();
		if (now < 0)
			return -1;
		int64_t remaining = deadline - now;
		if (remaining <= 0)
			return 1;
		struct timespec timeout = {
		    .tv_sec = remaining / 1000000000,
		    .tv_nsec = remaining % 1000000000,
		};
		/* SIGCHLD remains blocked between waitpid and this wait. */
		if (sigtimedwait(signals, NULL, &timeout) < 0) {
			if (errno == EAGAIN)
				return 1;
			if (errno != EINTR)
				return -1;
		}
	}
}

static int exec_shell(const char *shell, const char *script, int inline_script,
		      const char *directory, const char *module_id, char *const overrides[],
		      int detached, int64_t timeout, int *status)
{
	struct script_environment environment __attribute__((cleanup(free_environment))) = {0};
	if (script_environment(&environment, module_id, overrides))
		return -1;
	int64_t deadline = timeout;
	if (timeout > 0) {
		int64_t now = ksu_now_ns();
		if (now < 0)
			return -1;
		deadline += now;
	}
	sigset_t signals, previous;
	sigemptyset(&signals);
	sigaddset(&signals, SIGCHLD);
	if (sigprocmask(SIG_BLOCK, &signals, &previous))
		return -1;

	int result = -1, pipe_fd[2] = {-1, -1};
	if (pipe2(pipe_fd, O_CLOEXEC))
		goto done;
	pid_t pid = fork();
	if (pid < 0)
		goto done;
	if (!pid) {
		close(pipe_fd[0]);
		if (sigprocmask(SIG_SETMASK, &previous, NULL))
			goto child_error;
		if (detached) {
			ksu_detach_process_group(1);
			ksu_switch_cgroups();
		}
		if (directory && chdir(directory))
			goto child_error;
		char *argv[] = {(char *)shell, "sh", inline_script ? "-c" : (char *)script,
				inline_script ? (char *)script : NULL, NULL};
		execve(shell, argv, environment.values);
	child_error:
		int error = errno;
		while (write(pipe_fd[1], &error, sizeof(error)) < 0 && errno == EINTR) {
		}
		_exit(127);
	}

	close(pipe_fd[1]);
	pipe_fd[1] = -1;
	int child_error;
	ssize_t received;
	do {
		received = read(pipe_fd[0], &child_error, sizeof(child_error));
	} while (received < 0 && errno == EINTR);
	if (received > 0) {
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
		}
		errno = child_error;
	} else if (!received) {
		/* Expired stages still launch later scripts, without waiting or killing them. */
		result = 0;
		if (timeout < 0)
			result = wait_script(pid, &signals, -1, status);
		else if (timeout > 0) {
			int64_t now = ksu_now_ns();
			if (now < 0)
				result = -1;
			else if (now < deadline)
				result = wait_script(pid, &signals, deadline, status);
		}
	}
done:
	int error = errno;
	if (pipe_fd[0] >= 0)
		close(pipe_fd[0]);
	if (pipe_fd[1] >= 0)
		close(pipe_fd[1]);
	if (sigprocmask(SIG_SETMASK, &previous, NULL))
		__android_log_print(ANDROID_LOG_WARN, "KernelSU",
				    "Failed to restore signal mask: %s", strerror(errno));
	errno = error;
	return result;
}

int ksu_exec_script(const char *shell, const char *path, const char *directory,
		    const char *module_id, char *const overrides[], int64_t timeout)
{
	return exec_shell(shell, path, 0, directory, module_id, overrides, 1, timeout, NULL);
}

int ksu_exec_shell(const char *shell, const char *script, int inline_script, const char *directory,
		   const char *module_id, char *const overrides[], int *status)
{
	return exec_shell(shell, script, inline_script, directory, module_id, overrides, 0, -1,
			  status);
}

static int stage_deadline(int64_t *timeout)
{
	if (*timeout <= 0)
		return 0;
	int64_t now = ksu_now_ns();
	if (now < 0)
		return -1;
	*timeout += now;
	return 0;
}

static int exec_stage_file(const char *shell, const char *path, const char *directory,
			   const char *module_id, int64_t deadline)
{
	int64_t timeout = deadline;
	if (deadline > 0) {
		int64_t now = ksu_now_ns();
		if (now < 0)
			return -1;
		timeout = now < deadline ? deadline - now : 0;
	}
	__android_log_print(ANDROID_LOG_INFO, "KernelSU", "exec %s", path);
	int result = ksu_exec_script(shell, path, directory, module_id, NULL, timeout);
	if (result > 0)
		__android_log_print(ANDROID_LOG_WARN, "KernelSU",
				    "Timed out waiting for script: %s", path);
	return result < 0 ? -1 : 0;
}

int ksu_exec_common_scripts(const char *shell, const char *directory, int64_t timeout)
{
	struct stat st;
	if (stat(directory, &st)) {
		__android_log_print(ANDROID_LOG_INFO, "KernelSU", "%s not exists, skip", directory);
		return 0;
	}
	if (stage_deadline(&timeout))
		return -1;
	DIR *dir = opendir(directory);
	if (!dir)
		return -1;
	int result = 0;
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		char *path = ksu_join_path(directory, entry->d_name);
		if (!path) {
			result = -1;
			break;
		}
		if (stat(path, &st) || !S_ISREG(st.st_mode) || !(st.st_mode & 0111))
			__android_log_print(ANDROID_LOG_WARN, "KernelSU",
					    "%s is not executable, skip", path);
		else
			result = exec_stage_file(shell, path, directory, NULL, timeout);
		free(path);
		if (result < 0)
			break;
	}
	int error = errno;
	closedir(dir);
	errno = error;
	return result;
}

int ksu_exec_module_stage(const char *shell, const char *directory, const char *metamodule_link,
			  const char *stage, int64_t timeout)
{
	if (stage_deadline(&timeout))
		return -1;
	char *metamodule = ksu_find_metamodule(metamodule_link, directory);
	char *resolved_meta = metamodule ? realpath(metamodule, NULL) : NULL;
	free(metamodule);
	char *script_name = NULL;
	DIR *dir = opendir(directory);
	int result = -1;
	if (!dir || asprintf(&script_name, "%s.sh", stage) < 0)
		goto done;
	result = 0;
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		char *path = ksu_join_path(directory, entry->d_name);
		char *script = NULL, *resolved = NULL;
		if (!path) {
			result = -1;
			break;
		}
		int active = ksu_module_active(path);
		if (active < 0) {
			result = -1;
			goto next;
		}
		if (!active)
			goto next;
		if (resolved_meta) {
			resolved = realpath(path, NULL);
			if (resolved && !strcmp(resolved, resolved_meta))
				goto next;
		}
		script = ksu_join_path(path, script_name);
		if (!script) {
			result = -1;
			goto next;
		}
		struct stat st;
		if (stat(script, &st))
			goto next;
		const char *module_id = NULL;
		if (ksu_valid_utf8(entry->d_name, strlen(entry->d_name))) {
			if (ksu_valid_identifier((const unsigned char *)entry->d_name,
						 strlen(entry->d_name)))
				module_id = entry->d_name;
			else
				__android_log_print(
				    ANDROID_LOG_WARN, "KernelSU",
				    "Invalid module ID '%s' extracted from script path '%s'",
				    entry->d_name, script);
		}
		result = exec_stage_file(shell, script, path, module_id, timeout);
	next:
		free(resolved);
		free(script);
		free(path);
		if (result < 0)
			break;
	}
done:
	int error = errno;
	if (dir)
		closedir(dir);
	free(script_name);
	free(resolved_meta);
	errno = error;
	return result;
}
