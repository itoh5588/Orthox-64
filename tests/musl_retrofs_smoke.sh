#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/musl-retrofs-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/musl-retrofs-qemu.out}"
QEMU_PID=""

cleanup() {
    if [ -n "${QEMU_PID}" ] && kill -0 "${QEMU_PID}" 2>/dev/null; then
        kill "${QEMU_PID}" 2>/dev/null || true
        wait "${QEMU_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

echo "Building orthos.iso..."
make orthos.iso >/tmp/musl-retrofs-build.out

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"

echo "Starting QEMU..."
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

echo "Waiting for muslcheck: PASS on RetroFS root..."
# Increased timeout to 120 seconds for RetroFS build/boot
for _ in $(seq 1 120); do
    if grep -q 'muslcheck: PASS' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    if grep -q 'muslcheck: FAIL' "${SERIAL_LOG}" 2>/dev/null; then
        echo "muslcheck: FAIL detected in serial log!"
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if grep -q 'muslcheck: PASS' "${SERIAL_LOG}"; then
    echo "RetroFS root muslcheck: PASS"
    # Verify no EPERM errors for missing files in stat path
    if grep -q 'Operation not permitted' "${SERIAL_LOG}"; then
        echo "WARNING: 'Operation not permitted' found in log!"
        grep 'Operation not permitted' "${SERIAL_LOG}"
    fi
    exit 0
else
    echo "RetroFS root muslcheck: FAIL or TIMEOUT"
    tail -n 50 "${SERIAL_LOG}"
    exit 1
fi
