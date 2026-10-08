#ifndef __KSU_H_ARCH
#define __KSU_H_ARCH

#ifndef __aarch64__
#error "KaSU requires ARM64"
#endif

#define PT_REGS_SYSCALL_PARM1(x) ((x)->regs[0])
#define PT_REGS_PARM2(x) ((x)->regs[1])
#define PT_REGS_PARM3(x) ((x)->regs[2])
#define PT_REGS_SYSCALL_PARM4(x) ((x)->regs[3])
#define PT_REGS_PARM5(x) ((x)->regs[4])
#define PT_REGS_PARM6(x) ((x)->regs[5])

#endif
