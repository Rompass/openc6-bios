#!/usr/bin/env bash
# Flash a previously built OpenC6 BIOS image.
#
# Usage:
#   scripts/flash.sh [port] [timeout_seconds]
#
#   port             Serial port. Auto-detects a /dev/cu.usbmodem* or
#                     /dev/cu.usbserial* device if omitted.
#   timeout_seconds  How long to keep retrying if the port isn't there yet
#                     or drops mid-attempt (default: 30). Useful on boards
#                     like the ESP32-C3 SuperMini that can briefly drop off
#                     USB during a brownout/reset cycle — see
#                     ESP32-C3-SUPERMINI-PORT.md for why.
#
# Requires ESP-IDF to be sourced first: . $IDF_PATH/export.sh
# Run scripts/build.sh first — this only flashes an existing build/ output.

set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [ -z "${IDF_PATH:-}" ]; then
    echo "error: IDF_PATH is not set. Run '. \$IDF_PATH/export.sh' first." >&2
    exit 1
fi

if [ ! -f build/flasher_args.json ]; then
    echo "error: no build output found. Run scripts/build.sh first." >&2
    exit 1
fi

PORT="${1:-}"
TIMEOUT="${2:-30}"

find_port() {
    # `command ls` avoids breakage if the user's shell aliases `ls` to
    # something like eza/exa that adds color codes / icons to the output.
    command ls /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null | head -1
}

if [ -z "$PORT" ]; then
    PORT="$(find_port || true)"
fi

end=$((SECONDS + TIMEOUT))
attempt=0
while [ $SECONDS -lt "$end" ]; do
    attempt=$((attempt + 1))
    if [ -z "$PORT" ]; then
        PORT="$(find_port || true)"
    fi
    if [ -z "$PORT" ]; then
        sleep 0.2
        continue
    fi

    echo "==> Attempt $attempt: flashing $PORT"
    if idf.py -p "$PORT" flash; then
        echo "==> Flash succeeded on attempt $attempt"
        exit 0
    fi

    echo "==> Attempt $attempt failed, retrying..."
    PORT=""
    sleep 0.2
done

echo "error: could not flash within ${TIMEOUT}s. If the board keeps dropping" >&2
echo "off USB, this is likely a power delivery issue, not a script bug --" >&2
echo "see the Power section in ESP32-C3-SUPERMINI-PORT.md." >&2
exit 1
