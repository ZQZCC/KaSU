/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __KSU_TINYFS_SUCOMPAT_H
#define __KSU_TINYFS_SUCOMPAT_H

#ifdef CONFIG_KSU_TINYFS_SUCOMPAT
bool ksu_tinyfs_sucompat_ready(void);
void ksu_tinyfs_sucompat_init(void);
#endif

#endif
