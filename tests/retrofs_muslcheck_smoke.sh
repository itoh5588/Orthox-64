#!/bin/bash
set -euo pipefail

ISO="${1:-orthos.iso}"
SERIAL_LOG="${SERIAL_LOG:-LOGs/retrofs-muslcheck-serial.log}"
mkdir -p LOGs
QEMU_OUT="${QEMU_OUT:-/tmp/retrofs-muslcheck-qemu.out}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/retrofs_muslcheck_smoke.ash"
BOOTCMD_BACKUP="$(mktemp)"
SCRIPT_BACKUP="$(mktemp)"
QEMU_PID=""

cleanup() {
    if [ -n "${QEMU_PID}" ] && kill -0 "${QEMU_PID}" 2>/dev/null; then
        kill "${QEMU_PID}" 2>/dev/null || true
        wait "${QEMU_PID}" 2>/dev/null || true
    fi
    if [ -f "${BOOTCMD_BACKUP}" ]; then
        cp "${BOOTCMD_BACKUP}" "${BOOTCMD_PATH}"
        rm -f "${BOOTCMD_BACKUP}"
    fi
    if [ -f "${SCRIPT_BACKUP}" ]; then
        if [ -s "${SCRIPT_BACKUP}" ]; then
            cp "${SCRIPT_BACKUP}" "${SCRIPT_PATH}"
        else
            rm -f "${SCRIPT_PATH}"
        fi
        rm -f "${SCRIPT_BACKUP}"
    fi
}
trap cleanup EXIT

cp "${BOOTCMD_PATH}" "${BOOTCMD_BACKUP}"
if [ -f "${SCRIPT_PATH}" ]; then
    cp "${SCRIPT_PATH}" "${SCRIPT_BACKUP}"
else
    : > "${SCRIPT_BACKUP}"
fi

cat > "${SCRIPT_PATH}" <<'EOF'
echo retrofs-muslcheck-bootcmd-start
echo retrofs-muslcheck-bootcmd-ok
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/retrofs_muslcheck_smoke.ash
EOF

make orthos.iso >/tmp/retrofs-muslcheck-build.out

rm -f "${SERIAL_LOG}" "${QEMU_OUT}"

qemu-system-x86_64 \
    -machine pc \
    -cpu qemu64 \
    -m 2G \
    -cdrom "${ISO}" \
    -boot d \
    -display none \
    -serial "file:${SERIAL_LOG}" \
    -k en-us >"${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

echo "Waiting for RetroFS muslcheck smoke..."
for _ in $(seq 1 90); do
    if grep -q 'retrofs-muslcheck-bootcmd-ok' "${SERIAL_LOG}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

grep -q 'Welcome to Orthox-64 Shell!' "${SERIAL_LOG}"
grep -q 'stat(/non-existent-file-xyz) ret=-1 errno=2' "${SERIAL_LOG}"
grep -q 'muslcheck: PASS' "${SERIAL_LOG}"
grep -q "File: '/muslcheck.dir/hello.copy'" "${SERIAL_LOG}"
grep -q 'retrofs-muslcheck-bootcmd-start' "${SERIAL_LOG}"
grep -q 'retrofs-muslcheck-bootcmd-ok' "${SERIAL_LOG}"

echo "RetroFS muslcheck smoke PASS"
tail -n 200 "${SERIAL_LOG}"
