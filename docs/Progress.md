# Progress

Port log for this repo. The project master log is
KernelSU-Next-AVD/docs/progress.md, rounds there map to rounds here.

## Round 1, 2026-10-14: repo and first CI run

- Cloned inforcqb/susfs4ksu-lkm at 85ef920, full history, pushed to a new
  private repo ymuuuu/susfs4ksu-lkm-x64. A clone in a new repo, not a GitHub
  fork, because forks of public repos cannot be private.
- Remotes: origin is upstream, fork is this repo.
- One workflow, two jobs. arm64-control builds the tree exactly like upstream
  CI. x86_64-port prepares the x86_64 kdir in-job following the
  KernelSU-Next ddk-lkm-ksu.yml recipe, then builds the module. Upstream's
  four workflows removed, they live in git history.
- Run 38574360220: control green, port red. The unmodified arm64 source
  built for x86_64 gave the measured worklist: 63 pt_regs errors in 10 files
  (55 regs[N], 8 pc), one arm64 struct stat layout assert in susfs_kstat.c,
  one fatal include of arm64-only asm/memory.h in imports_guard.c.
- Found on the way: upstream .gitignore line 42 ignores every new *.md, docs
  must be force-added.

## Round 2, 2026-10-14: imports_guard and patch_memory

- imports_guard.c: gated asm/memory.h and the memstart_addr plus
  arm64_use_ng_mappings entries behind the arch. Verified first that nothing
  else uses those symbols and that the emulator kallsyms has native_set_fixmap
  but no __set_fixmap or set_fixmap, the x86 fixmap header routes the macro
  through a static inline to the global native_set_fixmap. Run 38576535178:
  imports_guard out of the error list, 64 errors left, control green.
- patch_memory.c: added the __x86_64__ branch adapted from KernelSU-Next
  hook/x86_64/patch_memory.c, the write path of the x86_64 kernelsu.ko
  already running on the target kernel. FIX_BTMAP_BEGIN, no-op cache flushes,
  scan_call_to stays a NULL stub like the reference, nothing here calls it.
  Run 38577629469: patch_memory compiled clean, same 63 pt_regs errors plus
  the kstat assert left, control green.
- Both runs kept the control green, so the ifdef splits did not break arm64.

## Round 3, 2026-10-14: docs and the pt_regs accessor

- docs/ folder created: ARCH.md, PLAN.md, Progress.md, REFs.md moved in.
  REFs.md stays canonical here, the AVD tool holds a marked snapshot.
- Usage map first, no blind rewrite: all argument reads are constant indices
  0 to 4 on four receiver names, one variable-index site inside the opath
  wrapper macro that passes the flags-argument position in as a parameter,
  and the 8 pc sites all read regs[30] as the return address.
- susfs_sc_arg accessor added to susfs_kprobe.h: arm64 regs[idx], x86_64
  di, si, dx, r10, r8, r9 by index, runtime index on purpose for opath.
  All constant-index reads rewritten with it, opath got the include it was
  missing. The regs[30] plus pc sites are untouched, they are round 4.
- sus_path.c and comment mentions now use the accessor name too, both stay
  truthful prose.
- CI for this step took three runs, two of them caught real mistakes:
  - Run 38579964450 failed BOTH jobs. The control caught a duplicated #endif
    my two-step header edit left behind. The x86 job caught that the opath
    wrapper macro's flags_reg access was never rewritten (designed as a
    runtime index, execution missed it, the leftover check only matched
    digit indices) and that the read rewrite had mangled argument WRITES
    into assignments to a function call.
  - Run 38580813759 added two accessors: susfs_set_sc_arg for genuine
    argument rewrites (avc_spoof rewrites a sid at an index that depends on
    the kernel version, open_redirect swaps a path pointer at index 0) and
    susfs_sc_set_ret for return values (arm64 writes x0 pre-skip, x86_64
    callers read ax). It still failed the control: susfs_supercall.c used
    the accessors without including the header, and the presence check that
    said otherwise was a string grep that matched a comment mention in that
    file, not an include line.
  - Run 38582052035: control green, x86 red on exactly the designed residue,
    8 pc sites with their regs[30] partners plus the kstat assert. Every
    accessor user is now checked against real include lines, and edited
    headers get a preprocessor balance count before push.
- Lessons logged: rewrite checks must cover writes and non-digit indices,
  include checks must match include lines, never string mentions.

## Decisions

- 2026-10-14: arm64 control job stays. Control experiment plus arm64
  regression guard, about two minutes per run, droppable once the port is
  stable.
- 2026-10-14: one commit per step, one CI run per commit, red port job while
  porting is the worklist, not an accident.
- 2026-10-14: the 8 pc sites get a design agreement before any code.
