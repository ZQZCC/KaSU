// SPDX-License-Identifier: GPL-2.0
#include "small_rt.h"

#include <sys/ioctl.h>
#include <fcntl.h>

#define KSU_IOCTL_TINYFS_GET_SU_FD _IO('K', 0xf0)
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
	int su_fd;

	argv[0] = "su";
	su_fd = raw_syscall4(SYS_openat, AT_FDCWD, (long)"/system/bin/su",
			     O_RDONLY | O_CLOEXEC, 0);
	if (su_fd < 0)
		goto fail;
	fd = raw_syscall3(SYS_ioctl, su_fd, KSU_IOCTL_TINYFS_GET_SU_FD, 0);
	raw_syscall3(SYS_close, su_fd, 0, 0);
	if (fd < 0)
		goto fail;

	if (raw_syscall3(SYS_ioctl, fd, KSU_IOCTL_GRANT_ROOT, 0) < 0)
		goto fail;

	raw_syscall3(SYS_execve, (long)ksud, (long)argv, (long)envp);
	raw_syscall3(SYS_execve, (long)shell, (long)argv, (long)envp);

fail:
	__builtin_trap();
}
