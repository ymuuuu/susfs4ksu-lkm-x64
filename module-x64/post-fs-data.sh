#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Load the x86_64 susfs_guard_lkm .ko once, at the earliest boot stage.
# Mechanics mirror upstream's arm64 wrapper: skip if disabled or already
# up, match the kernel release, load through ksud, log everything.
# The module ships one .ko per KMI, named androidNN-M.N_susfs_guard_lkm.ko;
# the KMI is derived from the kernel release, whose GKI spelling is
# x.y.z-androidNN-k..., so 6.6.66-android15-8 maps to android15-6.6.
MODDIR=${0%/*}
[ -f "$MODDIR/disable" ] && exit 0
[ -f "$MODDIR/remove" ] && exit 0
umask 077
exec >>"$MODDIR/load.log" 2>&1
echo "susfs_lkm_x64 loader: $(date)"
if [ -e /proc/susfs_path ]; then
  echo "susfs already present; nothing to do."
  exit 0
fi
REL=$(cat /proc/sys/kernel/osrelease) || { echo "cannot read kernel release"; exit 1; }
KVER=$(echo "$REL" | cut -d- -f1 | cut -d. -f1,2)
KTAG=$(echo "$REL" | sed -n 's/.*-\(android[0-9][0-9]*\)-.*/\1/p')
KMI="$KTAG-$KVER"
case "$(uname -m)-$KMI" in
  x86_64-android12-5.10|x86_64-android13-5.10|x86_64-android13-5.15|x86_64-android14-5.15|x86_64-android14-6.1|x86_64-android15-6.6|x86_64-android16-6.12|x86_64-android17-6.18) ;;
  *) echo "no x86_64 susfs build for this kernel: $REL (derived KMI $KMI); refusing"; exit 1 ;;
esac
KO="$MODDIR/${KMI}_susfs_guard_lkm.ko"
[ -f "$KO" ] || { echo "module for $KMI missing from the module dir:"; ls "$MODDIR"; exit 1; }
/data/adb/ksud insmod "$KO"
rc=$?
[ "$rc" = 0 ] && echo "loaded $KMI module." || echo "ksud insmod failed: $rc"
exit "$rc"
