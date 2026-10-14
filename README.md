# susfs4ksu-lkm-x64

x86_64 port of [inforcqb/susfs4ksu-lkm](https://github.com/inforcqb/susfs4ksu-lkm): susfs as a loadable kernel module for the Android 16 x86_64 emulator image, whose kernel comes from the android15-6.6 branch (6.6.66-android15-8), running beside KernelSU-Next LKM. No custom kernel, nothing replaces the ramdisk. Everything builds on GitHub Actions, nothing builds locally.

## Build

Pushes to main run two jobs: `arm64-control` (upstream module, unchanged, catches regressions) and `x86_64-port` (this port). Both must be green. Artifacts per run: `x86_64-susfs_guard_lkm.ko` and `x86_64-susfs_sc` (bare supercall test client).

## Load

Plain `insmod` cannot work: five imports are unexported to modules on every GKI variant, arm64 included, same as upstream. Use the documented loader:

```sh
ksud insmod /data/local/tmp/susfs_guard_lkm.ko
```

The module hides itself from lsmod on purpose; check `/sys/module/susfs_guard_lkm` instead. Rollback is a plain reboot.

## State

Live-tested on the emulator: `ksud susfs version` answers v2.3.0 and `ksud susfs support` answers Supported, sus_path hides paths from app uids while root and shell still see them, uname spoof, kstat/opath/open_redirect tables, no-opt probes holding. v1 scope: the AArch32 compat layer stays arm64-only.

Userspace talks through the reboot-ABI supercall and the `/proc/susfs_*` nodes (interface reference: upstream [PROC_INTERFACES.md](https://github.com/inforcqb/susfs4ksu-lkm/blob/main/PROC_INTERFACES.md)). Manager modules that ship arm64 `susfs`/`ksu_susfs` binaries (BRENE, sidex15) need the planned x86_64 CLI bridge.

## Origin and license

Port of inforcqb/susfs4ksu-lkm, which ports simonpunk/susfs4ksu (logic and hook points), with patch_memory/lsm_hook/symbol_resolver from SukiSU-Ultra. GPL-2.0-only, see LICENSE. Research and learning only, use at your own risk.
