#ifndef __KSU_H_KSU
#define __KSU_H_KSU

#define KERNEL_SU_VERSION KSU_VERSION

struct cred* ksu_cred;

#if defined(CONFIG_KSU_DEBUG) || defined(CONFIG_KSU_SHELL_HAS_SU_ALWAYS)
static bool allow_shell = true;
#else
static bool allow_shell = false;
#endif

extern struct cred* ksu_cred;

#endif
