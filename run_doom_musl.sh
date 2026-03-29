#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ISO_PATH="$SCRIPT_DIR/out/orthos.iso"
LOG_DIR="$SCRIPT_DIR/LOGs"
MONITOR_SOCK="/tmp/orthox-qemu-doom-musl.sock"
SERIAL_LOG="$LOG_DIR/serial_doom_musl.log"

cd "$SCRIPT_DIR"
mkdir -p "$LOG_DIR"
rm -f "$MONITOR_SOCK" "$SERIAL_LOG"

cleanup() {
    rm -f "$MONITOR_SOCK"
}
trap cleanup EXIT

qemu-system-x86_64 \
    -M pc,pcspk-audiodev=audio0 \
    -audiodev coreaudio,id=audio0 \
    -device sb16,audiodev=audio0 \
    -cpu max \
    -m 2G \
    -cdrom "$ISO_PATH" \
    -boot d \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCK,server,nowait" \
    -no-reboot &
QEMU_PID=$!

python3 <<'PY'
import socket
import time
from pathlib import Path

sock = None
serial_log = Path("LOGs/serial_doom_musl.log")
for _ in range(400):
    try:
        if "Welcome to Orthox-64 Shell!" in serial_log.read_text(errors="ignore"):
            break
    except FileNotFoundError:
        pass
    time.sleep(0.1)
else:
    raise SystemExit("shell did not start")

for _ in range(100):
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect("/tmp/orthox-qemu-doom-musl.sock")
        sock.settimeout(0.2)
        break
    except OSError:
        time.sleep(0.1)
else:
    raise SystemExit("monitor connect failed")

try:
    try:
        sock.recv(1024)
    except Exception:
        pass

    mapping = {
        "/": "slash",
        "-": "minus",
        ".": "dot",
        "\n": "ret",
    }

    def send(cmd):
        sock.sendall(cmd.encode("utf-8") + b"\n")
        try:
            sock.recv(1024)
        except Exception:
            pass

    time.sleep(0.5)
    for ch in "/bin/doom-musl.elf\n":
        send(f"sendkey {mapping.get(ch, ch)}")
        time.sleep(0.12)
finally:
    sock.close()
PY

wait "$QEMU_PID"
