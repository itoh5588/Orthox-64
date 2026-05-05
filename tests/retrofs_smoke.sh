#!/bin/bash
set -euo pipefail

ISO="${1:-orthos-retrofs.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/retrofs-smoke-serial.log}"
mkdir -p LOGs
MONITOR_SOCK="${MONITOR_SOCK:-retrofs-smoke.sock}"
QEMU_OUT="${QEMU_OUT:-/tmp/retrofs-smoke-qemu.out}"
QEMU_PID=""

cleanup() {
    if [ -n "${QEMU_PID}" ] && kill -0 "${QEMU_PID}" 2>/dev/null; then
        kill "${QEMU_PID}" 2>/dev/null || true
        wait "${QEMU_PID}" 2>/dev/null || true
    fi
    rm -f "${MONITOR_SOCK}"
}
trap cleanup EXIT

rm -f "${SERIAL_LOG}" "${MONITOR_SOCK}" "${QEMU_OUT}"

qemu-system-x86_64 \
    -machine q35 \
    -cpu qemu64 \
    -m 2G \
    -bios Web/wasabi/third_party/ovmf/RELEASEX64_OVMF.fd \
    -cdrom "${ISO}" \
    -boot d \
    -display none \
    -audio none \
    -serial "file:${SERIAL_LOG}" \
    -monitor "unix:${MONITOR_SOCK},server,nowait" \
    -k en-us >"${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

echo "Waiting for shell prompt..."
for _ in $(seq 1 90); do
    if grep -q 'Welcome to Orthox-64 Shell!' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

if ! grep -q 'Welcome to Orthox-64 Shell!' "${SERIAL_LOG}" 2>/dev/null; then
    echo "Shell did not start"
    tail -n 120 "${SERIAL_LOG}" 2>/dev/null || true
    exit 1
fi

python3 <<'PY'
import socket
import time
import os

SOCK = os.environ.get("MONITOR_SOCK", "retrofs-smoke.sock")
cmd_env = os.environ.get("RETROFS_CMDS", "")
if cmd_env:
    CMDS = [line + "\n" for line in cmd_env.splitlines() if line]
else:
    CMDS = [
        "ls /\n",
        "/bin/retrofsbasic\n",
        "/bin/retrofsedge\n",
        "/bin/musldircheck\n",
    ]

KEYMAP = {
    '/': 'slash',
    '.': 'dot',
    '-': 'minus',
    '_': 'shift-minus',
    ';': 'semicolon',
    ':': 'shift-semicolon',
    ' ': 'spc',
    '(': 'shift-9',
    ')': 'shift-0',
    '"': 'shift-apostrophe',
}

def send_key(sock, ch):
    if ch in ("\n", "\r"):
        sock.sendall(b"sendkey ret\n")
        return
    key = KEYMAP.get(ch, ch)
    sock.sendall(f"sendkey {key}\n".encode())

sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
for _ in range(40):
    try:
        sock.connect(SOCK)
        break
    except OSError:
        time.sleep(0.25)
else:
    raise SystemExit("monitor connect failed")

time.sleep(1.0)
for cmd in CMDS:
    for ch in cmd:
        send_key(sock, ch)
        time.sleep(0.08)
    time.sleep(2.0)
sock.close()
PY

sleep "${RETROFS_SETTLE_SECONDS:-4}"
tail -n 200 "${SERIAL_LOG}"
