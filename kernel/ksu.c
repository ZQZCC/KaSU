#ifdef MODULE
#error "KaSU requires built-in TinyFS support"
#endif

#include "kernel_includes.h"

// selinux includes
#include "avc_ss.h"
#include "objsec.h"
#include "ss/services.h"
#include "ss/symtab.h"
#include "xfrm.h"
#ifndef KSU_COMPAT_USE_SELINUX_STATE
#include "avc.h"
#endif

// uapi
#include "include/uapi/app_profile.h"
#include "include/uapi/feature.h"
#include "include/uapi/selinux.h"
#include "include/uapi/supercall.h"
#include "include/uapi/sulog.h"

// includes
#include "include/arch.h"
#include "include/klog.h"
#include "include/ksu.h"

// kernel compat
#include "kernel_compat.h"
#include "include/util.h"

#include "policy/app_profile.h"
#include "policy/allowlist.h"
#include "policy/feature.h"
#include "manager/apk_sign.h"
#include "manager/manager_identity.h"
#include "manager/throne_tracker.h"
#include "supercall/internal.h"
#include "supercall/supercall.h"
#include "infra/su_mount_ns.h"
#include "infra/file_wrapper.h"
#include "infra/event_queue.h"
#include "feature/kernel_umount.h"
#include "feature/selinux_hide.h"
#include "feature/sucompat.h"
#include "feature/sulog.h"
#include "hook/tinyfs_sucompat.h"
#include "runtime/ksud.h"
#include "sulog/event.h"
#include "sulog/fd.h"

#include "selinux/selinux.h"
#include "selinux/sepolicy.h"

#ifdef CONFIG_KPROBES
#include "downstream/kprobes_common.h"
#endif

#ifdef CONFIG_KALLSYMS
#include "external/chibihash64.h"
#include "downstream/kallsyms_common.h"
#endif

#ifdef CONFIG_ARM64
#include "downstream/arm64_branch_insn.h"
#endif

#include "downstream/slow_avc_audit_defs.h"
#include "downstream/tiny_sulog.h"
#include "downstream/toolkit.h"
#include "downstream/vmap_patch.h"

// unity build
#include "policy/allowlist.c"
#ifdef CONFIG_KSU_USERSPACE_POLICY
#include "policy/policy_store.c"
#endif
#include "policy/app_profile.c"
#include "policy/feature.c"
#include "manager/apk_sign.c"
#include "manager/pkg_observer.c"
#include "manager/throne_tracker.c"

#include "supercall/perm.c"
#include "supercall/dispatch.c"
#include "supercall/supercall.c"

#include "infra/su_mount_ns.c"
#include "infra/file_wrapper.c"
#include "infra/event_queue.c"

#include "feature/selinux_hide.c"
#include "feature/sucompat.c"
#ifdef CONFIG_KSU_TINYFS_SUCOMPAT
#include "hook/tinyfs_sucompat.c"
#endif
#include "feature/sulog.c"
#include "runtime/ksud.c"

#include "sulog/event.c"
#include "sulog/fd.c"

#include "hook/lsm_hooks_manual.c"

#include "selinux/selinux.c"
#include "selinux/sepolicy.c"
#include "selinux/rules.c"

// track backports and other quirks here
// ref: kernel_compat.c, Makefile
// yes looks nasty
#if defined(CONFIG_KSU_DEBUG)
	#define FEAT_1 " +debug"
#else
	#define FEAT_1 ""
#endif
#if defined(CONFIG_KSU_EXTRAS)
	#define FEAT_3 " +extras"
#else
	#define FEAT_3 ""
#endif
#define FEAT_5 " +manual_hooks"
#if defined(KSU_COMPAT_HAS_EXPORTED_POLICY_RWLOCK)
	#define FEAT_7 " +policy_rwlock"
#else
	#define FEAT_7 ""
#endif
#if defined(CONFIG_KSU_TINYFS_SUCOMPAT)
	#define FEAT_9 " +tinyfs_sucompat"
#else
	#define FEAT_9 ""
#endif
#define EXTRA_FEATURES FEAT_1 FEAT_3 FEAT_5 FEAT_7 FEAT_9

static inline void ksu_print_build_info(void)
{
	pr_info("welcome to KernelSU version " __stringify(KERNEL_SU_VERSION) ", package name " KSU_PACKAGE_NAME "\n");
	pr_info("Initialized on: %s (%s)%s\n", UTS_RELEASE, UTS_MACHINE, EXTRA_FEATURES);

#if defined(__VERSION__) && defined(__STDC_VERSION__)
#if defined(__clang_version__)
	pr_info("Built with: Clang %d.%d.%d w/ stdc: %ld\n", __clang_major__, __clang_minor__, __clang_patchlevel__, __STDC_VERSION__);
#else
	pr_info("Built with: GCC %s w/ stdc: %ld\n", __VERSION__, __STDC_VERSION__);
#endif
#endif

}

static int __init kernelsu_init(void)
{
	ksu_print_build_info();

#ifdef CONFIG_KSU_DEBUG
	pr_alert("*************************************************************");
	pr_alert("**     NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE    **");
	pr_alert("**                                                         **");
	pr_alert("**         You are running KernelSU in DEBUG mode          **");
	pr_alert("**                                                         **");
	pr_alert("**     NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE NOTICE    **");
	pr_alert("*************************************************************");
#endif
	if (allow_shell)
		pr_alert("shell is allowed at init!");

	ksu_cred = prepare_creds();
	if (!ksu_cred) {
		pr_err("prepare cred failed!\n");
		return -ENOSYS;
	}

	ksu_feature_init();

	ksu_supercalls_init();

	ksu_sucompat_init(); // so the feature is registered

	ksu_selinux_hide_init(); // so the feature is registered

#ifdef CONFIG_KSU_FEATURE_SULOG	
	ksu_sulog_init(); // so the feature is registered
#endif

	ksu_core_init();

	ksu_allowlist_init();

	ksu_throne_tracker_init();

	ksu_ksud_init();

	ksu_file_wrapper_init();

	return 0;
}

device_initcall(kernelsu_init);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("weishu");
MODULE_DESCRIPTION("Android KernelSU");
