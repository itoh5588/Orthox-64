#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ISO_PATH="$SCRIPT_DIR/out/orthos.iso"
SERIAL_LOG="$SCRIPT_DIR/LOGs/serial_doom.log"

mkdir -p "$SCRIPT_DIR/LOGs"
(
sleep 3
echo "sendkey slash"
sleep 0.1
echo "sendkey b"
sleep 0.1
echo "sendkey o"
sleep 0.1
echo "sendkey o"
sleep 0.1
echo "sendkey t"
sleep 0.1
echo "sendkey slash"
sleep 0.1
echo "sendkey d"
sleep 0.1
echo "sendkey o"
sleep 0.1
echo "sendkey o"
sleep 0.1
echo "sendkey m"
sleep 0.1
echo "sendkey dot"
sleep 0.1
echo "sendkey e"
sleep 0.1
echo "sendkey l"
sleep 0.1
echo "sendkey f"
sleep 0.1
echo "sendkey ret"
sleep 15
) | qemu-system-x86_64 -M pc -cpu max -m 2G -cdrom "$ISO_PATH" -boot d -display none -serial "file:$SERIAL_LOG" -monitor stdio
