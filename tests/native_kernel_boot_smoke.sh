#!/bin/bash
# native_kernel_boot_smoke.sh
# Two-phase test:
#   Phase 1 – Build kernel natively inside Orthox-64, save result to RetroFS disk.
#   Phase 2 – Boot the saved native kernel.elf in a fresh QEMU and verify it runs.
set -euo pipefail

ISO="${1:-orthos-retrofs.iso}"
ROOTFS_IMG="${ROOTFS_IMG:-rootfs.img}"
SERIAL_LOG_BUILD="${SERIAL_LOG_BUILD:-native-kernel-boot-build.log}"
SERIAL_LOG_BOOT="${SERIAL_LOG_BOOT:-native-kernel-boot-serial.log}"
QEMU_OUT="${QEMU_OUT:-/tmp/native-kernel-boot-qemu.out}"
BOOT_ISO="${BOOT_ISO:-native-kernel-boot.iso}"
NATIVE_KERNEL="native-kernel.elf"
KOUT_IMG="kernel-output.img"

BOOTCMD_PATH="rootfs/etc/bootcmd"
SCRIPT_PATH="rootfs/etc/native_kernel_build_smoke.sh"
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
    rm -rf native_boot_iso_root
    rm -f "${KOUT_IMG}"
}
trap cleanup EXIT

cp "${BOOTCMD_PATH}" "${BOOTCMD_BACKUP}"
if [ -f "${SCRIPT_PATH}" ]; then
    cp "${SCRIPT_PATH}" "${SCRIPT_BACKUP}"
else
    : > "${SCRIPT_BACKUP}"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Phase 1: build kernel natively; copy result to RetroFS so it persists on disk
# ─────────────────────────────────────────────────────────────────────────────
echo "=== Phase 1: native build ==="

cat > "${SCRIPT_PATH}" <<'EOF'
export PATH=/bin:/usr/bin:/
echo native-kernel-build-start
mkdir /tmp/kbuild
mkdir /tmp/kbuild/kernel
mkdir /tmp/kbuild/lwip
mkdir /tmp/kbuild/lwip/core
mkdir /tmp/kbuild/lwip/core/ipv4
mkdir /tmp/kbuild/lwip/netif
make -f /src/kernel-build/Makefile BUILD=/tmp/kbuild OUTPUT=/tmp/kernel.elf
cp /tmp/kernel.elf /dev/kout
echo native-kernel-saved
EOF

cat > "${BOOTCMD_PATH}" <<'EOF'
/bin/ash /etc/native_kernel_build_smoke.sh
EOF

echo "Building ${ISO}..."
make "${ISO}" > /tmp/native-kernel-boot-iso-build.out 2>&1

dd if=/dev/zero bs=1M count=16 of="${KOUT_IMG}" 2>/dev/null
rm -f "${SERIAL_LOG_BUILD}" "${QEMU_OUT}"

QEMU_ARGS=(
    -M q35
    -cpu max
    -m 2G
    -cdrom "${ISO}"
    -boot d
    -display none
    -serial "file:${SERIAL_LOG_BUILD}"
    -no-reboot
    -drive if=none,id=rootfs,file="${ROOTFS_IMG}",format=raw
    -device virtio-blk-pci,drive=rootfs
    -drive if=none,id=kout,file="${KOUT_IMG}",format=raw
    -device virtio-blk-pci,drive=kout
)

echo "Running QEMU for native build (timeout 3600s)..."
qemu-system-x86_64 "${QEMU_ARGS[@]}" > "${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

for _ in $(seq 1 3600); do
    if grep -q "native-kernel-saved" "${SERIAL_LOG_BUILD}" 2>/dev/null; then
        break
    fi
    if grep -q "EXCEPTION OCCURRED\|#PF(User):\|#UD" "${SERIAL_LOG_BUILD}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if ! grep -q "native-kernel-saved" "${SERIAL_LOG_BUILD}" 2>/dev/null; then
    echo "Phase 1 failed (kernel not saved). Build log tail:"
    tail -n 30 "${SERIAL_LOG_BUILD}"
    exit 1
fi
echo "Phase 1 passed: native kernel saved to RetroFS."

# ─────────────────────────────────────────────────────────────────────────────
# Phase 2: extract native-kernel.elf from rootfs.img, build boot ISO, verify
# ─────────────────────────────────────────────────────────────────────────────
echo "=== Phase 2: extract and boot ==="

python3 - "${NATIVE_KERNEL}" "${KOUT_IMG}" <<'PYEOF'
import struct, sys
out_path = sys.argv[1]
img_path = sys.argv[2]
with open(img_path, "rb") as f:
    data = f.read()
if data[:4] != b'\x7fELF':
    print(f"ERROR: no ELF magic in {img_path}", file=sys.stderr)
    sys.exit(1)
e_phoff     = struct.unpack_from('<Q', data, 32)[0]
e_phentsize = struct.unpack_from('<H', data, 54)[0]
e_phnum     = struct.unpack_from('<H', data, 56)[0]
e_shoff     = struct.unpack_from('<Q', data, 40)[0]
e_shentsize = struct.unpack_from('<H', data, 58)[0]
e_shnum     = struct.unpack_from('<H', data, 60)[0]
end = 64
for i in range(e_phnum):
    base = e_phoff + i * e_phentsize
    p_offset = struct.unpack_from('<Q', data, base + 8)[0]
    p_filesz  = struct.unpack_from('<Q', data, base + 32)[0]
    end = max(end, p_offset + p_filesz)
for i in range(e_shnum):
    base = e_shoff + i * e_shentsize
    sh_type   = struct.unpack_from('<I', data, base + 4)[0]
    sh_offset = struct.unpack_from('<Q', data, base + 24)[0]
    sh_size   = struct.unpack_from('<Q', data, base + 32)[0]
    if sh_type != 8:  # SHT_NOBITS
        end = max(end, sh_offset + sh_size)
end = max(end, e_shoff + e_shentsize * e_shnum)
with open(out_path, "wb") as f:
    f.write(data[:end])
print(f"Extracted ELF: {end} bytes -> {out_path}")
PYEOF

# Build boot ISO with native kernel inline (don't call make to avoid rootfs.img rebuild)
rm -rf native_boot_iso_root
mkdir -p native_boot_iso_root/boot/limine native_boot_iso_root/EFI/BOOT

cp "${NATIVE_KERNEL}"              native_boot_iso_root/boot/kernel.elf
cp user/sh.elf                     native_boot_iso_root/boot/sh.elf
cp "${ROOTFS_IMG}"                 native_boot_iso_root/boot/rootfs.img
cp iso/limine-retrofs.conf         native_boot_iso_root/boot/limine/limine.conf
cp limine/limine-bios.sys \
   limine/limine-bios-cd.bin \
   limine/limine-uefi-cd.bin       native_boot_iso_root/boot/limine/
cp limine/BOOTX64.EFI              native_boot_iso_root/EFI/BOOT/
cp limine/BOOTIA32.EFI             native_boot_iso_root/EFI/BOOT/

xorriso -as mkisofs -v -R -r -J \
    -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    native_boot_iso_root -o "${BOOT_ISO}" 2>/dev/null
limine/limine bios-install "${BOOT_ISO}"

echo "Built ${BOOT_ISO} ($(du -h "${BOOT_ISO}" | cut -f1))"

# Simple bootcmd for boot test
cat > "${BOOTCMD_PATH}" <<'EOF'
echo native-kernel-boot-ok
EOF

rm -f "${SERIAL_LOG_BOOT}"

QEMU_BOOT_ARGS=(
    -M q35
    -cpu max
    -m 2G
    -cdrom "${BOOT_ISO}"
    -boot d
    -display none
    -serial "file:${SERIAL_LOG_BOOT}"
    -no-reboot
    -drive if=none,id=rootfs,file="${ROOTFS_IMG}",format=raw
    -device virtio-blk-pci,drive=rootfs
)

echo "Booting native kernel (timeout 120s)..."
qemu-system-x86_64 "${QEMU_BOOT_ARGS[@]}" > "${QEMU_OUT}" 2>&1 &
QEMU_PID=$!

for _ in $(seq 1 120); do
    if grep -q "native-kernel-boot-ok\|muslcheck: PASS" "${SERIAL_LOG_BOOT}" 2>/dev/null; then
        break
    fi
    if grep -q "EXCEPTION OCCURRED\|#PF(User):\|#UD" "${SERIAL_LOG_BOOT}" 2>/dev/null; then
        break
    fi
    sleep 1
done

kill "${QEMU_PID}" 2>/dev/null || true
wait "${QEMU_PID}" 2>/dev/null || true
QEMU_PID=""

if grep -q "native-kernel-boot-ok\|muslcheck: PASS" "${SERIAL_LOG_BOOT}" 2>/dev/null; then
    echo "Test passed: native kernel boots successfully on Orthox-64."
    exit 0
fi

echo "Boot test failed. Serial log (tail):"
tail -n 50 "${SERIAL_LOG_BOOT}"
exit 1
