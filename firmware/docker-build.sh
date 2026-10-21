#!/usr/bin/env bash
# Build / merge / flash the StackChan home-link firmware without a native ESP-IDF install.
#   ./firmware/docker-build.sh build          # idf.py build inside espressif/idf:v5.5.4
#   ./firmware/docker-build.sh merge          # merge bootloader + partitions + app into one image
#   ./firmware/docker-build.sh flash <port>   # write merged image at 0x0 (device in download mode)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
IMAGE="${IDF_IMAGE:-espressif/idf:v5.5.4}"
cmd="${1:-build}"
case "$cmd" in
  build)
    mkdir -p "$HERE/.component-cache"
    docker run --rm -v "$ROOT":/project -w /project/firmware \
      -e HOME=/tmp -e IDF_COMPONENT_CACHE_PATH=/project/firmware/.component-cache \
      "$IMAGE" bash -lc "python3 fetch_repos.py && idf.py build"
    ;;
  merge)
    cd "$HERE/build"
    # flash_args: one "<offset> <file>" pair per line after the option line.
    args=()
    while read -r off file; do
      [[ "$off" == 0x* ]] && args+=("$off" "$file")
    done < flash_args
    esptool --chip esp32s3 merge-bin -o stack-chan-merged.bin --flash-mode dio --flash-size 16MB "${args[@]}"
    ls -l stack-chan-merged.bin
    ;;
  flash)
    # Writes each segment at its own offset. Never flash the merged image onto a configured
    # device: merge-bin pads the gaps with 0xFF and that wipes NVS (0x9000) = Wi-Fi, tokens, settings.
    port="${2:?usage: docker-build.sh flash <serial-port>}"
    cd "$HERE/build"
    args=()
    while read -r off file; do
      [[ "$off" == 0x* ]] && args+=("$off" "$file")
    done < flash_args
    esptool --chip esp32s3 -p "$port" -b 921600 write-flash "${args[@]}"
    ;;
  flash-full)
    # Factory-style full image (bootloader+partitions+app+assets, NVS region blank). Only for a
    # blank/erased device, or restore NVS from a backup afterwards (write-flash 0x9000 nvs.bin).
    port="${2:?usage: docker-build.sh flash-full <serial-port>}"
    esptool --chip esp32s3 -p "$port" -b 921600 write-flash 0x0 "$HERE/build/stack-chan-merged.bin"
    ;;
  *) echo "unknown command: $cmd" >&2; exit 2 ;;
esac
