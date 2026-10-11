// SPDX-License-Identifier: GPL-2.0
#include "small_rt.h"

#include <sys/ioctl.h>
#include <fcntl.h>

#define KSU_IOCTL_GRANT_ROOT _IOC(_IOC_NONE, 'K', 1, 0)

__attribute__((used))
void tinysu_main(long *stack)
{
	long argc = *stack;
	char **argv = (char **)(stack + 1);
	char **envp = argv + argc + 1;
	const char *ksud = "/data/adb/ksud";
	const char *shell = "/system/bin/sh";
	int fd;

	argv[0] = "su";
	fd = raw_syscall4(SYS_openat, AT_FDCWD, (long)"/system/bin/su", O_RDONLY, 0);
	if (fd < 0)
		goto fail;

	if (raw_syscall3(SYS_ioctl, fd, KSU_IOCTL_GRANT_ROOT, 0) < 0)
		goto fail;

	char name[14];
	char *program = name + sizeof(name) - 1;
	unsigned int number = fd;
	*program = '\0';
	do {
		*--program = '0' + number % 10;
	} while (number /= 10);
	*--program = ':';
	*--program = 'u';
	*--program = 's';
	argv[0] = program;
	raw_syscall3(SYS_execve, (long)ksud, (long)argv, (long)envp);
	argv[0] = "su";
	raw_syscall3(SYS_execve, (long)shell, (long)argv, (long)envp);

fail:
	__builtin_trap();
}
