# OVERVIEW

How susfs got running on the x86_64 Android emulator, in plain words.

## What this is

susfs is a kernel side hiding layer for rooted Android devices. Apps and root
detectors ask the kernel questions like "does this file exist", "what does
uname say", "which mounts are there". susfs sits between the kernel and those
questions and answers them the way a clean, unrooted device would.

Normally susfs is compiled into a custom kernel, which only exists for arm64
phones. The upstream project here (inforcqb's susfs4ksu-lkm) turned it into a
loadable module instead: no custom kernel, you load it at runtime. But that
module too was arm64 only. This repo is the port to x86_64, built for the
Android 16 emulator image (its kernel is from the android15-6.6
branch), so susfs runs next to KernelSU-Next on a plain
emulator with no kernel replacement at all.

## How the port was done

Everything was built on GitHub Actions only, never locally. The pipeline has
two jobs: one builds the unchanged arm64 module as a control, the other
builds the x86_64 one. Every step was one commit, one build run, and it only
counted when both jobs were green. The control job caught four real mistakes
over the rounds, which is exactly why it exists.

The hiding logic itself was not touched. sus_path, kstat spoofing, open
redirect, the LSM hooks, the mount scan: all of that is architecture blind
and came over as is. What changed was everything that touches the machine
directly:

1. Kernel entry point names differ per architecture, so every syscall the
   module hooks is now spelled per arch in one macro.
2. Registers differ: on arm64 a syscall's arguments and return value live in
   certain registers, on x86_64 in others. One small accessor layer handles
   both.
3. The skip mechanism. This is the heart of the supercall: when the module
   wants to answer a question itself, it must cancel the real syscall and
   return its own answer. On x86_64 the pre handler now writes the answer
   into the return register and points the instruction pointer at a tiny
   "return immediately" stub inside the module, the same idiom the kernel
   itself uses for its error injection feature. A canary self check proves
   this works before the module commits to loading, and refuses the load if
   it does not.
4. Text patching: the module's code that rewrites memory protections was
   written for arm64 page tables and got an x86_64 counterpart.
5. Small layout differences: one field of the file metadata struct sits at a
   different offset and width on x86_64, and the code landing pad bytes
   differ (endbr64 instead of bti). Both follow the arch now.
6. The 32 bit app compatibility layer stays arm64 only for this first
   version. The emulator runs 64 bit apps, so nothing is lost there today.

## Loading it

The module uses five kernel symbols that Android does not allow modules to
import, on any device, arm64 included. Upstream knows and documents this: the
module is meant to be loaded with `ksud insmod`, whose loader resolves those
addresses at load time. We load it the same way. Plain insmod will always
refuse, that is expected and not a bug.

## Two bugs on the way to first load

The first load attempt refused itself, by design, at the canary. Two short
diagnostic builds found the cause:

- The kernel side was innocent all along. It placed its trap instruction
  exactly where it should, including inside module code, and a twin canary
  on a core kernel function proved the skip mechanism worked end to end.
- The real bug was ours: the canary function returned a plain constant, and
  the compiler quietly replaced the call with the constant itself. No call
  ever ran, so the check could never pass. One volatile variable fixed it.
  That bug was architecture independent, it just had never been load tested
  anywhere.

## Proof it works, from the live emulator

- The supercall answers: version v2.3.0, variant GKI.
- uname spoof: set once, every user on the device sees the fake kernel
  string.
- sus_path: a folder under the rules is invisible to app uids ("No such
  file") while shell and root still see it.
- The probes hold their no optimization state, verified in the kernel's own
  probe list.
- The module hides itself from lsmod, on purpose, and a second insmod
  attempt is refused while it is loaded.

## Status and limits

Done: the port (B2), the first load (B3), and the core function tests (B4).

Not yet: the module does not survive a reboot, nothing loads it
automatically yet. That is B5, wiring it into the KernelSU-Next-AVD tool so
a booted emulator comes up with susfs active. Third party manager modules
built for arm64 phones (BRENE and friends) need a small x86_64 command line
translator to talk to this module, see Progress round 8 notes.

## Where everything is

- Code: this repo, kernel/ directory. All changes per arch are marked and
  commented.
- Builds: GitHub Actions, two jobs, artifacts on every run.
- The technical file map: ARCH.md. The plan and status: PLAN.md. The full
  round by round log: Progress.md. Restore point of the first green build:
  CHECKPOINT-1.md (git tag checkpoint-1-first-green). Sources we used:
  REFs.md.
