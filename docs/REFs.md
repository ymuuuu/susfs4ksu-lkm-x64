# REFs

Every link this project used or verified, grouped. Local paths included where
they are the working copy.

## Root solutions

- https://github.com/KernelSU-Next/KernelSU-Next
  Main object of study. dev branch read, v3.4.2 release assets checked for both
  the manager APK naming and the susfs module list.
- https://github.com/KernelSU-Next/KernelSU-Next/tree/dev
  The dev branch commits the whole susfs LKM research started from.
- https://github.com/tiann/KernelSU
  Original KernelSU. Its v3.3.1 module was the control in the CFI panic round,
  it panicked identically, which proved the fault was not KSUN specific.
- https://github.com/HChenX/KernelSU-AVD
  The AVD rooting script forked (cloned, not GitHub forked) into
  KernelSU-Next-AVD.

## susfs

- https://github.com/inforcqb/susfs4ksu-lkm
  The LKM susfs this port starts from. Upstream HEAD 85ef920, checked by
  ls-remote, no x86_64 work there.
- https://gitlab.com/simonpunk/susfs4ksu
  The original kernel patch based susfs, background for why the LKM form
  exists at all.
- https://github.com/matsuzaka-yuki/susfs-lkm-module
  Packaging wrapper around inforcqb releases, checked and set aside, it is not
  a second implementation.

## Build system

- https://github.com/5ec1cff/ddk
  The DDK tree generator. Linked from KernelSU-Next kernel/build-all-x64.sh.
  Its local mode comment is how the x86_64 kdir flow was found.
- ghcr.io/ylarod/ddk-min (container images, tags like android15-6.6-20260828)
  The CI container both projects build in. Our workflow uses the same image.
  The x86_64 kdir prep steps in .github/workflows/build.yml are taken from
  KernelSU-Next .github/workflows/ddk-lkm-ksu.yml, job "Build kernelsu.ko for
  x64".

## Blogs

- https://5ec1cff.github.io/my-blog/2024/01/16/avd-ksu/
  KernelSU on AVD, the original ramdisk plus module approach, January 2024.
- https://5ec1cff.github.io/my-blog/2024/01/31/avd-ksu2/
  The sequel post. Both validated during the research: the mechanism they
  describe is what ksud boot-patch --ramdisk automates today.

## Emulator internals

- https://issuetracker.google.com/issues/326479561
  The empty_data_disk behavior referenced while diagnosing app loss on old
  AVDs.
- https://android.googlesource.com/platform/external/qemu/
  Emulator source. android-qemu2-glue main.cpp createUserData settled how the
  empty data disk marker really runs, once, at AVD creation.

## Context and forums

- https://kernelsu.org/guide/installation.html
  LKM versus GKI integration mode docs.
- https://xdaforums.com/t/how-to-root-kernelsu-next-with-susfs.4745569
  Typical arm64 susfs flow, checked while confirming no x86_64 LKM port
  exists anywhere public.

## Local working copies

- /run/media/ymuu/Work/Android/KernelSU-Next-AVD
  Our fork of HChenX/KernelSU-AVD, plan A lives here, docs/PLAN.md and
  docs/progress.md are the project log.
- /run/media/ymuu/Work/Android/susfs4ksu-lkm-x64
  This repo, the x86_64 port.
- /run/media/ymuu/Work/Android/Shadow Hooking
  Research repo. refs/susfs4ksu-lkm-source is the frozen snapshot of
  inforcqb at 85ef920 used for the port survey.
- /home/ymuu/.zcode/workspace/default/ksun-dev
  KernelSU-Next dev clone used for all source claims (ksud, hook/x86_64,
  CI workflows).

## pc-site design round

- https://docs.kernel.org/trace/kprobes.html
  Authoritative pre_handler execution-path change rules, the jump-optimization
  warning, and the post_handler optimization veto.
- https://android.googlesource.com/kernel/common/+/refs/heads/android15-6.6/arch/x86/lib/error-inject.c
  override_function_with_return plus just_return_func, the ret-stub idiom the
  port copies.
- https://stackoverflow.com/questions/53457705/can-i-block-a-new-process-execution-using-kprobe
  Practical x86 skip discussion that matched the docs.
- https://lwn.net/Articles/375232
  Jump optimization mechanics, why post_handler blocks it.
- https://www.cnblogs.com/LiuYanYGZ/p/12644122.html and
  https://cloud.tencent.com/developer/article/1874723
  Chinese-language kprobe internals walk-throughs, register path to
  try_to_optimize_kprobe. CN search results otherwise mirrored the same
  English sources, no divergent local practice found.
