# ARCH

What this repo is: susfs as a loadable kernel module, originally arm64 only
(inforcqb/susfs4ksu-lkm at commit 85ef920, full history kept), ported here to
x86_64 for one target: the android15-6.6 emulator kernel. Root comes from
KernelSU-Next's own LKM, this module adds the hiding layer on top, at runtime,
with no kernel patches.

## Target

Verified live on the emulator kernel, 2026-10-14:

- 6.6.66-android15-8-gb66429556fb8-ab13070261, KMI android15-6.6, GKI
- built with clang r510928, kCFI on (`__cfi_*` symbols in kallsyms)
- every symbol the port needs is present: `__x64_sys_reboot`, `__x64_sys_statx`,
  `__x64_sys_newuname`, `__x64_sys_openat`, `__x64_sys_openat2`,
  `__x64_sys_newfstatat`, `__x64_sys_fstat`, `__x64_sys_getdents64`,
  `__x64_sys_faccessat`, `security_hook_heads`, `text_poke`, `text_poke_bp`,
  `native_set_fixmap`
- the emulator already runs the x86_64 kernelsu.ko built from the same
  container and recipe this repo uses

## How it loads

- Built out of tree against DDK kernel headers.
- The .ko carries strong SHN_UNDEF imports the kernel does not all export.
  `ksud insmod` and the bundled susfs_insmod absolutize them from kallsyms
  (SHN_UNDEF becomes SHN_ABS with the looked up address).
- imports_guard.c refuses a load where any import stayed at address zero, and
  cross-checks the data imports against the live kallsyms after the resolver
  is up.
- Everything not imported is resolved at runtime: symbol_resolver.c
  bootstraps kallsyms_lookup_name through register_kprobe, which is exported
  on the target kernel.

## Pieces, kernel/

| file | role | x86_64 status |
|---|---|---|
| patch_memory.c | RO/RX write primitive: fixmap alias plus copy_to_kernel_nofault under stop_machine | done, adapted from KernelSU-Next hook/x86_64, uses FIX_BTMAP_BEGIN |
| patch_memory.h | API and flags | had an x86_64 branch already |
| imports_guard.c | load-time import sanity, refuses zero addresses | done, imports native_set_fixmap |
| susfs_kprobe.h | kprobe re-arm helpers, susfs_sc_arg syscall argument accessor | accessor added, used by all hook files |
| susfs_supercall.c | command channel: kprobe on the reboot syscall wrapper, magic match, task_work deferral, LP64 ABI asserts | pending, the skip logic and symbol are arm64 |
| symbol_resolver.c | kallsyms bootstrap via register_kprobe | arch generic, untouched |
| lsm_hook.c | LSM hook install, security_hook_heads hlist path on 6.6 | arch generic, untouched |
| susfs_kstat.c | stat family hooks via patched syscall table slots | pending, arm64 struct stat layout assert |
| susfs_opath.c | openat family hooks via patched syscall table slots | pending, symbol names |
| susfs_uname.c | uname spoof via kretprobe on the newuname wrapper | pending, symbol and skip |
| sus_map.c | maps and mountinfo listing hooks | pending, pc sites |
| sus_mount.c | mount id and mount line hooks, statx kretprobe | pending, pc sites |
| sus_path.c | getdents layer with timing cover | code arch neutral after accessor, symbols pending |
| susfs_open_redirect.c | fd path redirect hooks | pending, pc sites |
| susfs_hide_syms.c | hides symbols from listing output | pending, pc sites |
| susfs_fp_hook.c | openat function-table hook, scans arm64 kCFI .cfi_jt thunks | pending investigation, x86_64 kCFI has no thunks |
| spoof_cmdline.c | cmdline spoof over saved_boot_config | pending review |
| susfs_avc_spoof.c | AVC log spoofing | pending review |
| susfs_enable_log.c | log toggle | pending review |
| ksu_umount_gate.c | umount gate for KernelSU interplay | pending review |

## Userspace ABI

The supercall structs are LP64 and asserted with BUILD_BUG_ON at build time,
so the same sizes hold on x86_64. The client side is arch generic:
`syscall(SYS_reboot, KSU_INSTALL_MAGIC1, SUSFS_MAGIC, cmd, &payload)`
(userspace/ksud/src/susfsd.rs in the KernelSU-Next tree).

## Build

GitHub Actions only, nothing builds locally.

- One workflow, .github/workflows/build.yml, container
  ghcr.io/ylarod/ddk-min:android15-6.6-20260828.
- arm64-control builds the tree the same way upstream CI does. It proves the
  pipeline and catches any ifdef split that breaks arm64. It is a control, not
  a deliverable.
- x86_64-port prepares the x86_64 kdir inside the job following the
  KernelSU-Next ddk-lkm-ksu.yml recipe (modpost patch, gki_defconfig, LTO
  off, modules_prepare, selinux built-in.a) and then builds the module.
- While the port is unfinished the x86_64-port job is red on purpose and the
  whole run shows red. That red is the worklist. Making it green early would
  be a false clean.

## Repo conventions

- Upstream .gitignore ignores every new *.md, docs are force-added.
- Docs live in docs/: ARCH.md (this file), PLAN.md, Progress.md, REFs.md.
- Progress.md is the port log. The KernelSU-Next-AVD tool's docs/progress.md
  stays the project master log.
