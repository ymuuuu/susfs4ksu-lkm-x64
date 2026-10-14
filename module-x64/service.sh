#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Late boot: retry the load if the early stage missed (other modules issue
# susfs rules from their own boot scripts, so being loaded by now matters),
# and publish the x86_64 susfs CLI under the names manager modules expect.
MODDIR=${0%/*}
[ -f "$MODDIR/disable" ] && exit 0
[ -f "$MODDIR/remove" ] && exit 0
umask 077
exec >>"$MODDIR/load.log" 2>&1
if [ ! -e /proc/susfs_path ]; then
  echo "service: susfs absent, retrying load"
  /data/adb/ksud insmod "$MODDIR/susfs_guard_lkm.ko" \
    && echo "service: loaded" || echo "service: load failed rc=$?"
fi
BIN_DIR=/data/adb/ksu/bin
mkdir -p "$BIN_DIR"
cp -f "$MODDIR/susfs_cli_x64" "$BIN_DIR/susfs"
chmod 755 "$BIN_DIR/susfs"
ln -sf susfs "$BIN_DIR/sus"
ln -sf susfs "$BIN_DIR/ksu_susfs"
echo "cli installed, version: $("$BIN_DIR/susfs" show version 2>&1)"
