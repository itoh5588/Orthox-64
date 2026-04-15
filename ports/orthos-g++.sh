#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYSROOT="${ORTHOS_SYSROOT:-$ROOT/user}"
INCLUDEDIR="${ORTHOS_INCLUDEDIR:-$SYSROOT/include}"
LIBDIR="$ROOT/user/libs"

raw_args=("$@")
args=()
link=true

for arg in "${raw_args[@]}"; do
    case "$arg" in
        -c|-E|-S)
            link=false
            ;;
        -lz|*/libz.so*)
            continue
            ;;
    esac
    args+=("$arg")
done

cmd=(
    g++
    -static
    -fno-PIC
    -fno-PIE
    -no-pie
    -fno-exceptions
    -fno-rtti
    -DHAVE_STDLIB_H
    -DHAVE_STRING_H
    -DHAVE_UNISTD_H
    -DHAVE_SYS_STAT_H
    -DHAVE_SYS_TYPES_H
    -DHAVE_LIMITS_H
    -L"$LIBDIR"
    "${args[@]}"
    -idirafter "$INCLUDEDIR"
)

if $link; then
    # Newlib/LLVM に必要なライブラリを明示的にリンク
    cmd+=(
        "$ROOT/user/crt0.o"
        "$ROOT/user/syscalls.o"
        "$ROOT/build/newlib/user/syscall_wrap.o"
        "$ROOT/ports/newlib_stubs.o"
        -lstdc++
        -lm
        -lc
    )
fi

exec "${cmd[@]}"
