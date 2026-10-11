# Quirks / Adaptations
## C-style
- GNU23, but written in a way compatible to GNU17/GNU11 compilers.
- pointer-heavy. assumes little endian on everything.
- metaprogramming is actually happening: compile-time overloading/reflection, RAII-like cleanup, template-like type trickery
- compat and backports are also done through that metaprogramming (mostly redefinitions)
- heavy use of compiler attributes / builtins, this is intended.
- minimum is GCC 4.9 / Clang 10

## build system
- unity build, single unit
- causes heavy inlining (high stack overflow risk)
- ensure inlining control (inline, noinline attributes)
- stack safety is disabled
- redefines str/mem fn's to builtins

## hooking
- wired up for aarch64 and armeabi, k3.0 ~ mainline (7.2 as of current)
- prefer syscalls and LSM always
- syscall table hooking is implemented
- ARM64 'branch-link', callsite inline hooking support for sucompat and 6.8+ LSM.
- real-deal-but-brittle kallsyms bruteforcer to hunt ksyms.
- manual hooking is still supported and will be kept forever.

## sucompat
- tweaked for downstream
- simd-like, last word first, per word compare
- sucompat gate is tweaked too

## LSM framework
- pure function pointer on sub 6.8
- 3.x LSM scans the whole kernel to hunt for selinux_ops.
- 4.2 ~ 6.8 relies on first list member hijack.
- 6.8+ LSM relies on branch link hooking. ARM64 only.
- manual hooking also available.

## Package authorization cleanup
- Observe successful packages.list replacements through /data/system's inode callback.
- One static work item waits for VFS rename completion, then reads a temporary text snapshot.
- Prune only after a complete valid read, serialized with app-profile updates.
- Keep no installed-package database; ordinary authorization remains a UID hash lookup.

## security_file_permission LSM
- we use this to avoid hooking sys_read for manual hooks
- after all we just need file pointer
- however if theres syscall table hook or kprobes_ksud, we hook it on there instead
- we also use this for "second stage apply" instead of execve_ksud

## bprm LSM
- defferent hooks for different kernels
- think of this as "after sys_execve"
- lockless argv pullouts for sulog
- might be used for something later

## selinux_hide
- we have a thin implementation downstream
- no kallsyms reliance, we hunt file operations instead, we try to keep this if possible.

## safe mode
- the implementation accepts 3x VOLUME_UP or 3x VOLUNE_DOWN to trigger safemode
- we have a dedicated input handler for this
- this will be disabled once ksud runs on_post_fs_data / ksu_is_safe_mode
- if theres no ksud to call it, it will disable itself 30s after init.rc load
- this should be enough allowance time from init.rc to post-fs-data

## kthreads
- theres a lot of these on the codebase even for mundane tasks

## 'We need to see the compiler as an enemy' section.
- constexpr is broken on clang < 19 even on -std=gnu23. IR generation fails.
- gcc 4.9 has issues on casted designated init. [related](https://github.com/torvalds/linux/commit/e8c07082a810fbb9db303a2b66b66b8d7e588b53)
- some have autistic inlining which will oom compilation for \__attribute\_\_((flatten)) (e.g. sultan). avoid.
- some kernel+compiler combo reads \__attribute\_\_((cold, noinline)) as __init, which evicts our fn. avoid.
- static_assert on fn ptr (e.g. static_assert(!!&fn);) on clang < 14? fails. compiler thinks it has to prove constness, it works like assert(!!__builtin_constant_p(fn)) NOT assert(__builtin_constant_p(!!fn))

