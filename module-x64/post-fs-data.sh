#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Load the x86_64 susfs_guard_lkm .ko once, at the earliest boot stage.
# Mechanics mirror upstream's arm64 wrapper: skip if disabled or already
# up, match the kernel release, load through ksud, log everything.
MODDIR=${0%/*}
[ -f "$MODDIR/disable" ] && exit 0
[ -f "$MODDIR/remove" ] && exit 0
umask 077
exec >"$MODDIR/load.log" 2>&1
echo "susfs_lkm_x64 loader: $(date)"
if [ -e /proc/susfs_path ]; then
  echo "susfs already present; nothing to do."
  exit 0
fi
REL=$(cat /proc/sys/kernel/osrelease) || { echo "cannot read kernel release"; exit 1; }
case "$(uname -m)-$REL" in
  x86_64-6.6.*-android15-*) ;;
  *) echo "not an android15-6.6 x86_64 kernel: $REL; refusing"; exit 1 ;;
esac
/data/adb/ksud insmod "$MODDIR/susfs_guard_lkm.ko"
rc=$?
[ "$rc" = 0 ] && echo "loaded." || echo "ksud insmod failed: $rc"
exit "$rc"
