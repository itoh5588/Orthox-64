#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/ports/gcc-4.7.4"
BUILD="$SRC/build-musl"
SYSROOT="${ORTHOS_SYSROOT:-$ROOT/ports/musl-install}"

export PATH="/opt/homebrew/opt/x86_64-elf-gcc/bin:$PATH"
export CC="$ROOT/ports/orthos-gcc.sh"
export CXX="$ROOT/ports/orthos-g++.sh"
export AR="x86_64-elf-ar"
export RANLIB="x86_64-elf-ranlib"

export ORTHOS_SYSROOT="$SYSROOT"
export ORTHOS_INCLUDEDIR="${ORTHOS_INCLUDEDIR:-$SYSROOT/include}"
export ORTHOS_LIBDIR="${ORTHOS_LIBDIR:-$SYSROOT/lib}"
export ORTHOS_CRT0="${ORTHOS_CRT0:-$ROOT/build/musl/user/crt0.o}"
export ORTHOS_SYSCALLS_O="${ORTHOS_SYSCALLS_O:-$ROOT/build/musl/user/syscalls_musl.o}"
export ORTHOS_SYSCALL_WRAP_O="${ORTHOS_SYSCALL_WRAP_O:-$ROOT/build/musl/user/syscall_wrap.o}"

mkdir -p "$BUILD"
cd "$BUILD"

export CFLAGS="-O2 -std=gnu89"
export CXXFLAGS="-O2 -std=gnu++98"
export LDFLAGS=""

export ac_cv_func_mmap=yes
export ac_cv_func_munmap=yes
export ac_cv_func_msync=yes
export ac_cv_func_sigaction=yes
export ac_cv_func_sigprocmask=yes
export ac_cv_func_waitpid=yes
export ac_cv_header_sys_mman_h=yes

if [ ! -f Makefile ]; then
  "$SRC"/configure \
    --target=x86_64-elf \
    --host=x86_64-elf \
    --prefix=/usr/local \
    --disable-nls \
    --enable-languages=c \
    --with-newlib \
    --with-sysroot="$SYSROOT" \
    --without-headers \
    --disable-shared \
    --disable-libssp \
    --disable-libgomp \
    --disable-libmudflap \
    --disable-libitm \
    --disable-libatomic \
    --disable-libquadmath \
    --disable-dependency-tracking \
    am_cv_CC_dependencies_compiler_type=gcc3 \
    am_cv_CXX_dependencies_compiler_type=gcc3 \
    gcc_cv_cxx_dependency_style=gcc3
fi

NCPU="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"

make -j"$NCPU" all-gcc
