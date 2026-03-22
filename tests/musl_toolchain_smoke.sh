#!/bin/bash
set -euo pipefail

QEMU_SOCK=/tmp/orthos-qemu-toolchain.sock
rm -f "$QEMU_SOCK" serial_toolchain.log

qemu-system-x86_64 -M pc -m 2G -cdrom orthos.iso -boot d -display none \
    -serial file:serial_toolchain.log -monitor unix:"$QEMU_SOCK",server,nowait -k en-us &
QEMU_PID=$!

cleanup() {
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    rm -f "$QEMU_SOCK"
}
trap cleanup EXIT

echo "Waiting for Orthox-64 Shell..."
for i in {1..40}; do
    if grep -q "Welcome to Orthox-64 Shell!" serial_toolchain.log 2>/dev/null; then
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
cat serial_toolchain.log
echo "---------------------"

grep -q -- "--- OrthOS GCC Pipeline Started ---" serial_toolchain.log
grep -q -- "\\[gcc\\] Executing /bin/cc1" serial_toolchain.log
grep -q -- "\\[gcc\\] Executing /bin/as" serial_toolchain.log
grep -q -- "\\[gcc\\] Executing /bin/ld" serial_toolchain.log
grep -q -- "--- OrthOS GCC Pipeline Finished! Output: a.out ---" serial_toolchain.log
grep -q -- "Hello, world" serial_toolchain.log
grep -q -- "\\[gcc\\] a.out returned: 0" serial_toolchain.log

echo "musl toolchain smoke test: PASS"
