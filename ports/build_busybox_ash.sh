#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT/ports/busybox}"
OUT="${2:-$ROOT/user/busybox-ash.elf}"
case "$OUT" in
  /*) ;;
  *) OUT="$ROOT/$OUT" ;;
esac

if [ ! -d "$SRC" ]; then
  echo "busybox source not found: $SRC" >&2
  exit 1
fi

export PATH="/opt/homebrew/bin:$PATH"
export LC_ALL=C
export CC="${ORTHOS_DRIVER:-$ROOT/ports/orthos-gcc.sh}"
export LD="${ORTHOS_DRIVER:-$ROOT/ports/orthos-gcc.sh}"
export AR="${ORTHOS_AR:-x86_64-elf-ar}"
export RANLIB="${ORTHOS_RANLIB:-x86_64-elf-ranlib}"
export STRIP="${ORTHOS_STRIP:-x86_64-elf-strip}"
INCLUDEDIR="${ORTHOS_INCLUDEDIR:-$ROOT/user/include}"
export CFLAGS="-O2 -DORTHOX_BUSYBOX_ASH_PTR_HACK=1 -I$INCLUDEDIR -Wno-format-security -Wno-stringop-overflow -Wno-unused-but-set-variable"
export HOSTCFLAGS="-Wno-format-security -Wno-stringop-overflow -Wno-unused-but-set-variable"
export LDFLAGS="-static -nostartfiles"
export LDLIBS=""

cd "$SRC"
rm -f .config
KCONFIG_ALLCONFIG="$ROOT/ports/busybox-ash.config" \
make CC="$CC" LD="$LD" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" allnoconfig
make CC="$CC" LD="$LD" AR="$AR" RANLIB="$RANLIB" STRIP="$STRIP" \
    -j"$(sysctl -n hw.ncpu)" busybox
cp busybox "$OUT"
