#!/bin/bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"
mkdir -p LOGs
QEMU_SOCK=/tmp/orthox-test-pipe.sock
SERIAL_LOG=LOGs/serial.log
rm -f "$QEMU_SOCK" "$SERIAL_LOG"
qemu-system-x86_64 -M pc -m 2G -cdrom out/orthos.iso -boot d -display none \
    -serial file:"$SERIAL_LOG" -monitor unix:"$QEMU_SOCK",server,nowait -k en-us &
QEMU_PID=$!

echo "Waiting for Orthox-64 Shell..."
for i in {1..30}; do
    if grep -q "Welcome to Orthox-64 Shell!" "$SERIAL_LOG" 2>/dev/null; then
        echo "Shell started!"
        break
    fi
    sleep 1
done

sleep 2
python3 << 'EOF'
import socket, time, sys

def send_qemu(cmd):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect("/tmp/orthox-test-pipe.sock")
        s.sendall(cmd.encode('utf-8') + b'\n')
        s.close()
    except:
        pass

def send_key(k):
    if k == ' ': k = 'spc'
    if k == '.': k = 'dot'
    if k == '/': k = 'slash'
    if k == '-': k = 'minus'
    if k == '_': k = 'shift-minus'
    send_qemu(f"sendkey {k}")
    time.sleep(0.1)

# ls
for c in "ls":
    send_key(c)
send_key('ret')
time.sleep(1)

# pipetest.elf
for c in "pipetest.elf":
    send_key(c)
send_key('ret')
time.sleep(10)
EOF

echo "--- Serial Output ---"
cat "$SERIAL_LOG"
echo "---------------------"

kill $QEMU_PID
wait $QEMU_PID 2>/dev/null
rm -f "$QEMU_SOCK"
