#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ISO_PATH="$SCRIPT_DIR/out/orthos.iso"

TTY_STATE=""
if [ -t 0 ]; then
    TTY_STATE="$(stty -g)"
    trap 'stty "$TTY_STATE"' EXIT INT TERM
    stty raw -echo
fi

exec qemu-system-x86_64 \
    -machine pc,pcspk-audiodev=audio0 \
    -audiodev coreaudio,id=audio0 \
    -device sb16,audiodev=audio0 \
    -cpu max \
    -m 2G \
    -cdrom "$ISO_PATH" \
    -boot d \
    "$@" \
    -serial stdio
