#!/bin/bash
set -euo pipefail

ISO="${1:-orthos-retrofs.iso}"
BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/native-python-smoke.sh"
BOOTCMD_BACKUP="$(mktemp)"
SCRIPT_BACKUP="$(mktemp)"
SERIAL_LOG="${SERIAL_LOG:-/tmp/retrofs-python-serial.log}"
MONITOR_SOCK="${MONITOR_SOCK:-/tmp/retrofs-python.sock}"
QEMU_OUT="${QEMU_OUT:-/tmp/retrofs-python-qemu.out}"
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
cat /dev/null > "${BOOTCMD_PATH}"

if [ -z "${GUEST_SMOKE_BODY:-}" ]; then
    echo "Error: GUEST_SMOKE_BODY is required" >&2
    exit 1
fi
cat > "${SCRIPT_PATH}" <<EOF
${GUEST_SMOKE_BODY}
EOF

make orthos.iso >/tmp/retrofs-python-build.out

export SERIAL_LOG
export MONITOR_SOCK
export QEMU_OUT
bash "$(dirname "$0")/retrofs_smoke.sh" "$ISO"

if [ -n "${EXPECTED_SERIAL_PATTERNS:-}" ]; then
    while IFS= read -r pattern; do
        if [ -z "${pattern}" ]; then
            continue
        fi
        if ! grep -q -- "${pattern}" "${SERIAL_LOG}" 2>/dev/null; then
            echo "Missing expected serial pattern: ${pattern}" >&2
            tail -n 200 "${SERIAL_LOG}" 2>/dev/null || true
            exit 1
        fi
    done <<EOF
${EXPECTED_SERIAL_PATTERNS}
EOF
fi
