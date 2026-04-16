#!/usr/bin/env bash
# ビルドして docs/installer/firmware/ にバイナリをコピー
# GitHub Pages の Web Serial インストーラ用
set -e
cd "$(dirname "$0")/.."

echo "=== Building firmware ==="
pio run

BUILD=.pio/build/m5stack-papers3
DEST=docs/installer/firmware

mkdir -p "$DEST"
cp "$BUILD/bootloader.bin"  "$DEST/"
cp "$BUILD/partitions.bin"  "$DEST/"
cp "$BUILD/firmware.bin"    "$DEST/"

# boot_app0.bin は framework から取得
BOOT_APP0=$(find .pio/build -name boot_app0.bin -print -quit 2>/dev/null)
if [ -n "$BOOT_APP0" ]; then
    cp "$BOOT_APP0" "$DEST/"
else
    echo "WARNING: boot_app0.bin not found, searching in framework..."
    BOOT_APP0=$(find ~/.platformio -name boot_app0.bin -print -quit 2>/dev/null)
    if [ -n "$BOOT_APP0" ]; then
        cp "$BOOT_APP0" "$DEST/"
    else
        echo "ERROR: boot_app0.bin not found!"
        exit 1
    fi
fi

echo ""
echo "=== Firmware files copied to $DEST ==="
ls -lh "$DEST"
echo ""
echo "Done! Commit and push to deploy via GitHub Pages."
