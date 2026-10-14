/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Arming a probe a second time needs the struct handed back the way the kernel left it the
 * first time.
 *
 * register_kprobe() resolves .symbol_name only while .addr is NULL: kprobe_addr() ->
 * _kprobe_addr() (kernel/kprobes.c) answers ERR_PTR(-EINVAL) as soon as both are set -
 *
 *     if ((symbol_name && addr) || (!symbol_name && !addr))
 *             goto invalid;
 *     ...
 * invalid:
 *     return ERR_PTR(-EINVAL);
 *
 * - and register_kprobe() stores the address it resolved BEFORE the checks that can still
 * fail it (`p->addr = addr;` sits above warn_kprobe_rereg()/check_kprobe_address_safe()), so
 * the field is written even when the call returns an error.  unregister_kprobes() clears it
 * only for a probe that was never on a list:
 *
 *     for (i = 0; i < num; i++)
 *             if (__unregister_kprobe_top(kps[i]) < 0)
 *                     kps[i]->addr = NULL;    (only on failure)
 *
 * which means a probe that DID arm keeps its address after it is removed.
 *
 * Put together: a struct kprobe that has armed once - or whose registration failed after the
 * symbol was resolved - can never be registered again.  Every later register_kprobe() on that
 * struct returns -EINVAL without touching the address, so the feature it carries stays off for
 * the rest of the module's life, and a failed retry looks exactly like a symbol that is not
 * probeable.
 *
 * Both sequences are ordinary here: every enable/disable control surface disarms and arms
 * again (the avc node, the sus_mount supercall), and every rule-management path retries a
 * registration that failed earlier (kstat's maps hook, open_redirect's forward and reverse
 * hooks, uname's set, sus_map's probes).  So both sides clear the field: the arm side before
 * it returns an error, the disarm side after unregistering.  sus_mount_fdinfo_arm()/disarm()
 * already did that by hand for its dynamically resolved addresses; these two helpers are the
 * same statement, so the next probe does not have to rediscover why it is needed.
 *
 * A kretprobe embeds its kprobe, hence the second helper.
 */
#ifndef _SUSFS_KPROBE_H
#define _SUSFS_KPROBE_H

#include <linux/kprobes.h>

/* Call AFTER unregister_kprobe()/unregister_kretprobe() returns, never before: unregister_kprobes()
 * runs each probe's bottom half only `if (kps[i]->addr)`, so clearing the field first would skip
 * __unregister_kprobe_bottom() and leave the probe's instruction slot behind.  Call it on every
 * registration failure too, for the reason in the block above. */
static inline void susfs_kp_forget_addr(struct kprobe *kp)
{
	kp->addr = NULL;
}

static inline void susfs_krp_forget_addr(struct kretprobe *krp)
{
	krp->kp.addr = NULL;
}

/* Syscall argument access.  Both supported arches dispatch syscalls through
 * wrappers that take one pt_regs, but the argument registers differ: arm64
 * keeps them in the regs[0..5] array (x0..x5), x86_64 uses named fields
 * (rdi, rsi, rdx, r10, r8, r9).  idx stays a runtime value because the
 * opath wrappers pass their flags-argument position in as a parameter. */
static __always_inline unsigned long susfs_sc_arg(const struct pt_regs *regs,
						  unsigned int idx)
{
#ifdef __aarch64__
	return regs->regs[idx];
#elif defined(__x86_64__)
	switch (idx) {
	case 0:
		return regs->di;
	case 1:
		return regs->si;
	case 2:
		return regs->dx;
	case 3:
		return regs->r10;
	case 4:
		return regs->r8;
	case 5:
		return regs->r9;
	default:
		return 0;
	}
#else
#error "susfs_sc_arg: unsupported arch"
#endif
}

/* Write a syscall argument in place, for the pre_handlers that rewrite one
 * before the real function runs (avc_spoof's sid, open_redirect's path).
 * Same index meaning as susfs_sc_arg. */
static __always_inline void susfs_set_sc_arg(struct pt_regs *regs,
					     unsigned int idx,
					     unsigned long val)
{
#ifdef __aarch64__
	regs->regs[idx] = val;
#elif defined(__x86_64__)
	switch (idx) {
	case 0:
		regs->di = val;
		return;
	case 1:
		regs->si = val;
		return;
	case 2:
		regs->dx = val;
		return;
	case 3:
		regs->r10 = val;
		return;
	case 4:
		regs->r8 = val;
		return;
	case 5:
		regs->r9 = val;
		return;
	}
#else
#error "susfs_set_sc_arg: unsupported arch"
#endif
}

/* Set the value a skipped syscall returns to its caller.  arm64 pre-skip
 * code writes x0 (regs[0]); x86_64 callers read the return out of ax. */
static __always_inline void susfs_sc_set_ret(struct pt_regs *regs,
					     unsigned long val)
{
#ifdef __aarch64__
	regs->regs[0] = val;
#elif defined(__x86_64__)
	regs->ax = val;
#else
#error "susfs_sc_set_ret: unsupported arch"
#endif
}

#endif /* _SUSFS_KPROBE_H */
