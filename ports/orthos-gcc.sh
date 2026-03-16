#!/bin/bash
set -euo pipefail

ROOT="/Users/itoh/orthOS-64"
SYSROOT="${ORTHOS_SYSROOT:-$ROOT/user}"
INCLUDEDIR="${ORTHOS_INCLUDEDIR:-$SYSROOT/include}"
LIBGCC="${ORTHOS_LIBGCC:-$(x86_64-elf-gcc -print-libgcc-file-name)}"
if [ -n "${ORTHOS_LIBDIR:-}" ]; then
    LIBDIR="$ORTHOS_LIBDIR"
elif [ -d "$SYSROOT/libs" ]; then
    LIBDIR="$SYSROOT/libs"
else
    LIBDIR="$SYSROOT/lib"
fi
CRT0="${ORTHOS_CRT0:-$ROOT/user/crt0.o}"
SYSCALLS_O="${ORTHOS_SYSCALLS_O:-$ROOT/user/syscalls.o}"
SYSCALL_WRAP_O="${ORTHOS_SYSCALL_WRAP_O:-$ROOT/build/newlib/user/syscall_wrap.o}"
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
        -lc)
            continue
            ;;
        -L*/user/libs|-L*/user/lib)
            continue
            ;;
        */user/crt0.o|*/build/*/user/crt0.o)
            continue
            ;;
        */user/syscalls.o|*/build/*/user/syscalls.o)
            continue
            ;;
        */user/syscall_wrap.o|*/build/*/user/syscall_wrap.o)
            continue
            ;;
    esac
    args+=("$arg")
done

cmd=(
    x86_64-elf-gcc
    -D__ORTHOS__
    -DHAVE_STDLIB_H
    -DHAVE_STRING_H
    -DHAVE_UNISTD_H
    -DHAVE_SYS_STAT_H
    -DHAVE_SYS_TYPES_H
    -DHAVE_LIMITS_H
    "${args[@]}"
    -idirafter "$INCLUDEDIR"
)

if ! $compile_only && ! $reloc_link; then
    cmd+=(
        -nostdlib
        -Wl,--wrap=signal
        -Wl,-u,main
        "$CRT0"
        "$SYSCALLS_O"
        "$SYSCALL_WRAP_O"
        -L"$LIBDIR"
        "$LIBGCC"
        -lc
    )
fi

exec "${cmd[@]}"
