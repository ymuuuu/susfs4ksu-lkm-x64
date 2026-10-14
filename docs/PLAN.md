# PLAN

Goal: susfs_guard_lkm.ko built for x86_64 android15-6.6, loaded with
`ksud insmod` on the rooted emulator, answering the KernelSU-Next manager
through the supercall, with at least one hiding feature proven working.

Rules, carried over from the project:

1. We decide together, no solo decisions.
2. No guesses, every claim verified in source or artifacts.
3. No AI filler text, no em dashes.
4. Changes small and minimal.
5. Guard skills on every round, clean-code-guard for code, docs-guard for docs.
6. Every step documented in Progress.md and memory.
7. CI only, nothing builds locally.
8. Easy first, then deeper. One commit per step, one CI run per commit.

## Phases

- B1, build environment. DONE. Private repo ymuuuu/susfs4ksu-lkm-x64, one
  workflow with an arm64 control job and an x86_64 port job.
- B2, port, in progress. Worklist measured by CI, not estimated:
  1. DONE, imports_guard.c: asm/memory.h and two arm64 data symbols gated,
     x86_64 imports native_set_fixmap.
  2. DONE, patch_memory.c x86_64 branch adapted from KernelSU-Next
     hook/x86_64/patch_memory.c.
  3. DONE, susfs_sc_arg accessor in susfs_kprobe.h, all constant-index
     argument reads rewritten. The opath wrapper's flags-argument position
     passes through as an index, so the accessor takes a runtime index.
  4. NEXT, the 8 pc sites: `regs->pc = regs->regs[30]` is arm64's "skip the
     rest of the function, return to caller" idiom, paired with return value
     writes. x86_64 has no link register, this needs a redesign we agree on
     before coding. Candidates: emulate ret in the kprobe pre_handler
     (ip from the stack top, sp bump, ax as return value) or restructure the
     affected hooks as kretprobes.
  5. susfs_kstat.c: arm64 struct stat offsets asserted at build time
     (st_nlink at 20, x86_64 differs). Needs per-arch offsets for the fields
     the hiding code rewrites, not a deleted assert.
  6. Symbol renames, `__arm64_sys_*` to `__x64_sys_*`, compat entries
     dropped for v1. CI cannot catch wrong names, they are runtime kallsyms
     lookups, B3 verifies.
  7. Makefile: arm64-only checks gated, -fno-stack-protector for v1.
  8. fp_hook investigation: x86_64 kCFI has no .cfi_jt thunks.
- B3, build and load on the emulator. Push the .ko, `ksud insmod`, read
  dmesg. Debug rule: if the emulator hangs, capture the kernel console first
  with -show-kernel, change nothing until it is read. Rollback is a reboot,
  the ramdisk is untouched.
- B4, function test. Supercall answers through `ksud susfs version` and
  `support`, manager shows the susfs section, a sus_path or sus_mount entry
  hides from a probe app.
- B5, persistence and script integration in the KernelSU-Next-AVD tool.

## Risks

- kCFI rejects wrong indirect call types. Mitigation: clang r510928 from the
  container, same as the kernel, exact prototypes.
- Slot overlap: susfs patches sys_call_table entries while KernelSU-Next's
  x86_64 module both patches slots and patches the do_syscall_64 dispatcher.
  Which path wins per syscall is unverified, B3 and B4 must confirm su and
  hiding both work loaded together.
- The pc sites and fp_hook are the only places with no reference
  implementation to copy.
