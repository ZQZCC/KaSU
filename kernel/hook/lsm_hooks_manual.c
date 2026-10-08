// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 \xx
 *
 * This file is a downstream extension and NOT affiliated, endorsed by,
 * or maintained by the official KernelSU developers.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

int ksu_bprm_check(struct linux_binprm *bprm)
{
#ifdef CONFIG_KSU_FEATURE_SULOG
	ksu_sulog_emit_bprm((const char *)bprm->filename);
#endif
	return 0;
}

int ksu_file_permission(struct file *file, int mask)
{
#ifdef KSU_CAN_USE_JUMP_LABEL
	if (static_branch_likely(&ksud_vfs_read_key))
		ksu_install_rc_hook(file);
#else
	if (unlikely(ksu_vfs_read_hook))
		ksu_install_rc_hook(file);
#endif

	return 0;
}

// NOTE: 3-args only. arg1 not used on some kernels.
int ksu_hide_setprocattr(const char *name, void *value, size_t size)
{
	ksu_hide_setprocattr_inline(name, value, size);
	return 0;
}

static void __init ksu_core_init(void)
{
	pr_info("%s: Automated LSM hooking disabled. Make sure manual security hooks are implemented!\n", __func__);
}
