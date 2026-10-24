#!/usr/bin/env bash
# Build OpenC6 BIOS for a given target.
#
# Usage:
#   scripts/build.sh c6        # ESP32-C6 (default target, 8 MB flash)
#   scripts/build.sh c3        # ESP32-C3 SuperMini (4 MB flash)
#
# Requires ESP-IDF to be sourced first: . $IDF_PATH/export.sh
#
# Note: this always does a clean reconfigure (removes build/ and sdkconfig)
# to guarantee target/partition-table settings don't get stuck from a
# previous build for the other target.

set -euo pipefail

TARGET="${1:-c6}"
cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [ -z "${IDF_PATH:-}" ]; then
    echo "error: IDF_PATH is not set. Run '. \$IDF_PATH/export.sh' first." >&2
    exit 1
fi

case "$TARGET" in
    c6)
        IDF_TARGET=esp32c6
        FLASH_SIZE=8MB
        PARTITION_TABLE=partitions.csv
        ;;
    c3)
        IDF_TARGET=esp32c3
        FLASH_SIZE=4MB
        PARTITION_TABLE=partitions_c3_4mb.csv
        ;;
    *)
        echo "usage: $0 [c6|c3]" >&2
        exit 1
        ;;
esac

echo "==> Building for $IDF_TARGET ($PARTITION_TABLE, $FLASH_SIZE flash)"
rm -rf build sdkconfig
idf.py set-target "$IDF_TARGET"

# set-target regenerates sdkconfig from sdkconfig.defaults + target defaults;
# force the flash size / partition table explicitly in case they differ from
# whatever's currently in sdkconfig.defaults.
{
    echo "CONFIG_ESPTOOLPY_FLASHSIZE_${FLASH_SIZE}=y"
    echo "CONFIG_PARTITION_TABLE_CUSTOM=y"
    echo "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"$PARTITION_TABLE\""
} >> sdkconfig

idf.py build

echo "==> Build complete for $IDF_TARGET"
