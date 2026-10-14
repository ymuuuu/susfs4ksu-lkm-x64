# Checkpoint 1: first full x86_64 build

Date: 2026-10-14. This is the point the port became a buildable x86_64
module. Come back here with:

    git clone https://github.com/ymuuuu/susfs4ksu-lkm-x64.git
    cd susfs4ksu-lkm-x64
    git checkout checkpoint-1-first-green

## Facts

- Commit: fc3c6236282ef761ab4f71047a860abb36320786, tag
  `checkpoint-1-first-green`, pushed to the private repo.
- CI run 38594474214, both jobs green for the first time:
  `arm64-control` success, `x86_64-port` success.
- Artifact: `x86_64-susfs_guard_lkm.ko`, unstripped 2,077,904 bytes,
  GitHub artifact id 11909601778.
- Local copy: `builds/checkpoint-1/susfs_guard_lkm.ko`, sha256
  8981fc1036af172211190c062325106cfda739fc91f57b8dbd88ec6593bfc757.
  Not committed, binary, kept on disk. GitHub artifacts expire, this copy
  does not.
- Target kernel: 6.6.66-android15-8-gb66429556fb8-ab13070261, KMI
  android15-6.6, kCFI on, IBT on, OPTPROBES on with optimization sysctl 1,
  no ftrace, RETHUNK off, stack protector strong.

## What works at this checkpoint

- The whole tree compiles and links for x86_64 in the DDK container, and
  still compiles for arm64 (control green).
- In and working per design: x86_64 patch_memory branch (FIX_BTMAP_BEGIN),
  imports_guard with native_set_fixmap, susfs_sc_arg accessors, the
  susfs_kp_skip_return ret-stub skip mechanism, the optimization veto
  post_handler on all 13 skip probes, NOKPROBE on the 8 handlers, the
  supercall init canary with delayed recheck, per-arch kstat stat offsets.

## What does NOT work yet, on purpose

- The artifact refuses to load on the emulator: symbol tables still carry
  `__arm64_*` names, supercall init is fatal when its probe cannot register.
  Item 6, the renames, is the gate to the first load test (B3).
- compat (32-bit ABI) layers: dropped for v1, table entries removed with
  item 6.
- fp_hook and the two symbol validators still carry arm64 BTI scan logic,
  fixed with item 6.

## History to this point

Rounds 1 to 6 in Progress.md: repo and CI, imports_guard and patch_memory,
pt_regs accessors (three runs, two caught mistakes), pc-site design
(research, docs, two-round second-model review) and implementation
(two more caught mistakes), kstat offsets, green.
