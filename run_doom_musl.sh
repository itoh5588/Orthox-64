#!/bin/bash
set -euo pipefail

MONITOR_SOCK="qemu_doom_musl.sock"
SERIAL_LOG="serial_doom_musl.log"

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
    -cdrom orthos.iso \
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
serial_log = Path("serial_doom_musl.log")
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
        sock.connect("qemu_doom_musl.sock")
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
