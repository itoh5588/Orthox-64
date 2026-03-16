#!/bin/bash
set -euo pipefail

ROOT="/Users/itoh/orthOS-64"
SYSROOT="${ORTHOS_SYSROOT:-$ROOT/user}"
INCLUDEDIR="${ORTHOS_INCLUDEDIR:-$SYSROOT/include}"

exec x86_64-elf-g++ \
    -fno-exceptions \
    -fno-rtti \
    -DHAVE_STDLIB_H \
    -DHAVE_STRING_H \
    -DHAVE_UNISTD_H \
    -DHAVE_SYS_STAT_H \
    -DHAVE_SYS_TYPES_H \
    -DHAVE_LIMITS_H \
    "$@" \
    -idirafter "$INCLUDEDIR"
