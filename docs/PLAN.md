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
  4. DONE, the 8 pc sites, designed 2026-10-14 (research, docs, two-round second-model
     verification and a two-round review by a second model:
     - One helper, susfs_kp_skip_return(regs, val) in susfs_kprobe.h. arm64
       keeps pc = x30, x0 = val. x86_64 sets ax = val and ip = a module-text
       bare ret stub (the kernel's own idiom, arch/x86/lib/error-inject.c
       override_function_with_return). Never touch sp: on kernels with
       kprobes-on-ftrace the ftrace return path drops sp writes, and the stub
       works on every path.
     - Unconditional no-op post_handler on all eight skip probes. It is the
       only module-side way to veto jump optimization (CONFIG_OPTPROBES=y and
       /proc/sys/debug/kprobes-optimization=1 measured on the target), the
       flag lands on the aggregated probe so self-checking is impossible, and
       optimization is asynchronous, about 5 jiffies after registration, so
       an unguarded probe works briefly then silently stops.
     - NOKPROBE_SYMBOL on each handler: reentry past the second level ends in
       BUG() on the int3 path.
     - Known cost to measure in B3: the post_handler also disables boost, so
       hot primitives like walk_page_range pay int3 plus single-step on every
       non-matching call.
     - Site 1 keeps the table-wrapper route as fallback and gained an init
       canary: probe a module-local noinline function with the same skip
       handler, verify the forced return, refuse the load otherwise, re-check
       from delayed work. A failed recheck makes reboot_pre pass through.
     - Runs 38590607834 and 38593047573 landed it. Two of my own mistakes
       were caught on the way: I changed the noopt post_handler to return int
       from stale memory, the tree's kprobe_post_handler_t is void; and the
       canary flag was used before its declaration.
  5. DONE, susfs_kstat.c per-arch native offsets: only st_nlink differs
     (u32 at 20 on arm64, unsigned long at 16 on x86_64), offset, width and
     a width assert follow the arch, every other number still checked by the
     original static_asserts. Run 38594474214: FIRST FULL x86_64 BUILD, both
     jobs green, 2,077,904 bytes linked, artifact x86_64-susfs_guard_lkm.ko
     uploaded.
  6. DONE, run 38596373807 plus the unused-variable fix, both green.
     SUSFS_SC_WRAPPER_NAME spells wrapper symbols per arch, all native
     tables and the uname, reboot, statx probes use it, the whole AArch32
     compat half is compiled only on arm64, and the landing pad checks
     accept endbr64 on x86_64. CI still cannot catch wrong names, they are
     runtime kallsyms lookups, B3 verifies on the emulator.
  7. Makefile: downgraded to an evidence check. The build links with the
     inherited -fstack-protector-strong, so forcing it off is only needed if
     the loader chokes on the canary symbol. B3 checks nm -u on the artifact
     first.
  8. DONE by design: x86_64 kCFI has no .cfi_jt thunks, hide_syms' stub
     follower documents it never fires there, scan_call_to stays a NULL
     stub with zero callers, and the landing pad question was the real
     piece, fixed in item 6 with endbr64.
- B3, DONE, 2026-10-14. Loaded via `ksud insmod` (plain insmod can never
  work: five imports are unexported on every GKI variant, arm64 included,
  the README's loaders rewrite them to absolute addresses). Two bugs found
  and fixed on the way: the vermagic mismatch is a non-issue through ksud's
  loader, and the load-refusing canary was LLVM constant-propagating the
  canary call away, fixed with a volatile. Live state verified: module in
  /sys/module, /proc/susfs_* nodes up, supercall armed, kprobes at +4 past
  endbr64 with the optimization veto holding, delayed recheck passing.
  Details in Progress round 8. Debug rule kept for future loads: if the
  emulator hangs, kernel console first, change nothing until it is read.
  Rollback is a plain reboot, the ramdisk is untouched.
- B4, DONE, 2026-10-14. `ksud susfs version` answers v2.3.0, `ksud susfs
  support` answers Supported (ksud speaks the same reboot ABI natively).
  set_uname spoofed the kernel string for every user until reboot, sus_path
  hides a test folder from app uids (ENOENT) while shell and root still see
  it, version/variant answered through the repo's own x86_64 client
  (tools/susfs_sc_x64, CI artifact x86_64-susfs_sc). Manager section not
  screenshotted, ksud answering it is the same path the manager uses.
- B5, redefined 2026-10-14 by user decision: the KernelSU-Next-AVD tool stays
  standalone, no susfs integration inside it. Persistence lives on this repo's
  side, shape to be agreed (candidate: a module zip from this repo that runs
  `ksud insmod` at boot, like upstream's own wrapper). Separate plan item: an
  x86_64 `susfs` CLI bridge for third party manager modules that call the
  classic command line but ship arm64 binaries. Nothing chosen or built yet.

## Risks

- kCFI rejects wrong indirect call types. Mitigation: clang r510928 from the
  container, same as the kernel, exact prototypes.
- Slot overlap: susfs patches sys_call_table entries while KernelSU-Next's
  x86_64 module both patches slots and patches the do_syscall_64 dispatcher.
  Which path wins per syscall is unverified, B3 and B4 must confirm su and
  hiding both work loaded together.
- The pc sites and fp_hook are the only places with no reference
  implementation to copy.
