#!/usr/bin/env bash
# Flash a provisioned Pip device in one go.
# Usage: ./flash_device.sh pip-ella-01.csv [/dev/ttyACM0]
# Prereq: ESP-IDF exported (. $IDF_PATH/export.sh), run from the firmware/ dir.
set -euo pipefail
CSV="${1:?usage: flash_device.sh <device-nvs.csv> [port]}"
PORT="${2:-/dev/ttyACM0}"
NAME=$(basename "$CSV" .csv)

python "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" \
    generate "$CSV" "/tmp/$NAME-nvs.bin" 0x6000
idf.py -p "$PORT" build flash
esptool.py --port "$PORT" write_flash 0x9000 "/tmp/$NAME-nvs.bin"
echo "Done: firmware + identity for $NAME flashed. Power-cycle the device."
