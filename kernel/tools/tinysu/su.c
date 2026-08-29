// SPDX-License-Identifier: GPL-2.0
#include "small_rt.h"

#include <sys/ioctl.h>

#define KSU_INSTALL_MAGIC1 0xDEADBEEF
#define KSU_INSTALL_MAGIC2 0xCAFEBABE
#define KSU_IOCTL_GRANT_ROOT _IOC(_IOC_NONE, 'K', 1, 0)

__attribute__((used))
void tinysu_main(long *stack)
{
	long argc = *stack;
	char **argv = (char **)(stack + 1);
	char **envp = argv + argc + 1;
	const char *ksud = "/data/adb/ksud";
	const char *shell = "/system/bin/sh";
	int fd = 0;

	argv[0] = "su";
	raw_syscall4(SYS_reboot, KSU_INSTALL_MAGIC1, KSU_INSTALL_MAGIC2, 0,
		     (long)&fd);
	if (!fd)
		goto fail;

	if (raw_syscall3(SYS_ioctl, fd, KSU_IOCTL_GRANT_ROOT, 0) < 0)
		goto fail;

	raw_syscall3(SYS_execve, (long)ksud, (long)argv, (long)envp);
	raw_syscall3(SYS_execve, (long)shell, (long)argv, (long)envp);

fail:
	__builtin_trap();
}
