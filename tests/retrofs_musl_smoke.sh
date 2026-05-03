#!/bin/bash
set -euo pipefail
ISO="orthos.iso"
SERIAL_LOG="retrofs-musl-serial.log"
QEMU_OUT="/tmp/retrofs-musl-qemu.out"

make orthos.iso > /tmp/retrofs-musl-build.out 2>&1

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"

qemu-system-x86_64 \
    -machine pc \
    -cpu qemu64 \
    -m 2G \
    -cdrom "${ISO}" \
    -boot d \
    -display none \
    -audio none \
    -serial "file:${SERIAL_LOG}" \
    -k en-us >"${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

echo "Waiting for RetroFS muslcheck..."
for _ in $(seq 1 60); do
    if grep -q "muslcheck: PASS" "${SERIAL_LOG}" 2>/dev/null; then
        echo "Found muslcheck: PASS"
        break
    fi
    if grep -q "muslcheck: FAIL" "${SERIAL_LOG}" 2>/dev/null; then
        echo "Found muslcheck: FAIL"
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true

if grep -q "muslcheck: PASS" "${SERIAL_LOG}"; then
    echo "RETROFS MUSLCHECK PASS"
else
    echo "RETROFS MUSLCHECK FAIL"
    tail -n 50 "${SERIAL_LOG}"
fi
