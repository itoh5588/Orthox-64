#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"
mkdir -p LOGs
QEMU_SOCK=/tmp/orthox-tests-musl-smoke.sock
SERIAL_LOG=LOGs/serial.log
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
            s.connect("/tmp/orthox-tests-musl-smoke.sock")
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
        ("/bin/ash", 2.0),
        ("echo musl-smoke-ok", 1.0),
        ("pwd", 1.0),
        ("printenv PATH", 1.0),
        ("env", 1.0),
        ("printf smoke-printf", 1.0),
        ("cat /hello.txt", 1.0),
        ("head -n 1 /hello.txt", 1.0),
        ("tail -n 1 /hello.txt", 1.0),
        ("touch /smoke.txt", 1.0),
        ("mkdir /smokedir", 1.0),
        ("cp /hello.txt /smokedir/hellocopy.txt", 1.5),
        ("mv /smokedir/hellocopy.txt /smokedir/movedcopy.txt", 1.0),
        ("ls /smokedir", 1.0),
        ("chmod 600 /smokedir/movedcopy.txt", 1.0),
        ("stat /smokedir/movedcopy.txt", 1.0),
        ("cat /smokedir/movedcopy.txt", 1.0),
        ("rm /smokedir/movedcopy.txt", 1.0),
        ("rmdir /smokedir", 1.0),
        ("ls /", 1.5),
        ("wc /hello.txt", 1.0),
        ("stat /hello.txt", 1.0),
        ("attest-musl.elf", 1.5),
        ("ls /bin", 2.0),
        ("exit", 1.0),
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

grep -q "musl-smoke-ok" "$SERIAL_LOG"
grep -q "PATH=/bin:/:/usr/bin:/boot" "$SERIAL_LOG"
grep -q "PWD=/" "$SERIAL_LOG"
grep -q "smoke-printf" "$SERIAL_LOG"
grep -q "Hello from OrthOS TAR File System!" "$SERIAL_LOG"
grep -q "smoke.txt" "$SERIAL_LOG"
grep -q "/hello.txt" "$SERIAL_LOG"
grep -q "movedcopy.txt" "$SERIAL_LOG"
grep -q "0600/-rw-------" "$SERIAL_LOG"
grep -q "at_test: PASS" "$SERIAL_LOG"
grep -q "busybox" "$SERIAL_LOG"
grep -q "Goodbye!" "$SERIAL_LOG"

echo "musl smoke test: PASS"
