#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SYSROOT="${ORTHOS_SYSROOT:-$ROOT/user}"
INCLUDEDIR="${ORTHOS_INCLUDEDIR:-$SYSROOT/include}"
GCC_INCLUDEDIR="$(gcc -print-file-name=include)"
LIBGCC="$(gcc -print-libgcc-file-name)"
LIBGCC_DIR="$(dirname "$LIBGCC")"
LIBDIR="$ROOT/user/libs"

raw_args=("$@")
args=()
compile_only=false
reloc_link=false

for arg in "${raw_args[@]}"; do
    case "$arg" in
        -c|-E|-S)
            compile_only=true
            ;;
        -r|-Wl,-r)
            reloc_link=true
            ;;
        -lc|-lz|-lgcc_s|-lpthread|-ldl|-lrt|*/libz.so*|*/libgcc_s.so*)
            continue
            ;;
        */crt0.o|*/crti.o|*/crtn.o|*/crtbegin*.o|*/crtend*.o|*/rcrt1.o)
            continue
            ;;
    esac
    args+=("$arg")
done

cmd=(
    gcc
    -static
    -fno-PIC
    -fno-PIE
    -no-pie
    -D__ORTHOS__
    -nostdinc
    -I"$INCLUDEDIR"
    -I"$GCC_INCLUDEDIR"
    -L"$LIBDIR"
    -L"$LIBGCC_DIR"
    "${args[@]}"
)

if ! $compile_only && ! $reloc_link; then
    cmd+=(
        -nostdlib
        -Wl,--wrap=signal
        -Wl,-u,main
        "$ROOT/user/crt0.o"
        -Wl,--start-group
        "$ROOT/user/tls.o" "$ROOT/user/arch_prctl.o"
        "$ROOT/ports/rust_stubs.o"
        "$ROOT/ports/newlib_stubs.o"
        "$ROOT/user/syscalls.o"
        "$ROOT/build/newlib/user/syscall_wrap.o"
        "$LIBGCC"
        -lstdc++
        -lm
        -lc
        -Wl,--end-group
    )
fi

exec "${cmd[@]}"
