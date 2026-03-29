#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs
QEMU_SOCK=/tmp/orthos-qemu-toolchain.sock
SERIAL_LOG=LOGs/serial_toolchain.log
rm -f "$QEMU_SOCK" "$SERIAL_LOG"

qemu-system-x86_64 -M pc -m 2G -cdrom out/orthos.iso -boot d -display none \
    -serial file:"$SERIAL_LOG" -monitor unix:"$QEMU_SOCK",server,nowait -k en-us &
QEMU_PID=$!

cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    rm -f "$QEMU_SOCK"
}
trap cleanup EXIT

echo "Waiting for Orthox-64 Shell..."
for i in {1..40}; do
    if grep -q "Welcome to Orthox-64 Shell!" "$SERIAL_LOG" 2>/dev/null; then
        echo "Shell started"
        break
    fi
    sleep 1
done

python3 <<'PY'
import socket
import time

def connect_monitor():
    for _ in range(50):
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect("/tmp/orthos-qemu-toolchain.sock")
            s.settimeout(0.2)
            break
        except OSError:
            time.sleep(0.1)
    else:
        raise SystemExit("monitor connect failed")
    try:
        s.recv(1024)
    except Exception:
        pass
    return s

def send_qemu(sock, cmd):
    sock.sendall(cmd.encode("utf-8") + b"\n")
    try:
        sock.recv(1024)
    except Exception:
        pass

def send_key(sock, ch):
    mapping = {
        " ": "spc",
        ".": "dot",
        "/": "slash",
        "-": "minus",
        "_": "shift-minus",
        ":": "shift-semicolon",
        "\n": "ret",
    }
    send_qemu(sock, f"sendkey {mapping.get(ch, ch)}")
    time.sleep(0.08)

def send_text(sock, text):
    for ch in text:
        send_key(sock, ch)
    send_key(sock, "\n")

time.sleep(2)
monitor = connect_monitor()
try:
    for cmd, delay in [
        ("gcc.elf hello.c", 25.0),
        ("ls /", 2.0),
        ("exit", 1.0),
    ]:
        send_text(monitor, cmd)
        time.sleep(delay)
finally:
    monitor.close()
PY

echo "--- Serial Output ---"
cat "$SERIAL_LOG"
echo "---------------------"

grep -q -- "--- OrthOS GCC Pipeline Started ---" "$SERIAL_LOG"
grep -q -- "\\[gcc\\] Executing /bin/cc1" "$SERIAL_LOG"
grep -q -- "\\[gcc\\] Executing /bin/as" "$SERIAL_LOG"
grep -q -- "\\[gcc\\] Executing /bin/ld" "$SERIAL_LOG"
grep -q -- "--- OrthOS GCC Pipeline Finished! Output: a.out ---" "$SERIAL_LOG"
grep -q -- "Hello, world" "$SERIAL_LOG"
grep -q -- "\\[gcc\\] a.out returned: 0" "$SERIAL_LOG"

echo "musl toolchain smoke test: PASS"
