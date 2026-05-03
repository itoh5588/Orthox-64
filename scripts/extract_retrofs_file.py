#!/usr/bin/env python3
"""Extract a single file from a RetroFS disk image."""
import struct
import sys
from pathlib import Path

SECTOR_SIZE = 512
MAX_NAME = 128
ENTRY_SIZE = SECTOR_SIZE // 2  # 256 bytes per directory entry
RFS_ID = 0x3153466F72746552     # "RetroFS1"
FLAG_DIR_START = 0x0004
FLAG_DIRECTORY = 0x0001

# Directory entry layout (256 bytes):
#   +0   uint32  flags
#   +4   char[128] name/title
#   +132 uint64  sector_start  (dir: parent_sector)
#   +140 uint64  length        (dir: dir_sectors count)
#   +148 uint64  sector_length (dir: continuation)
DIR_ENTRY_FMT = "<I128sQQQ"
DIR_ENTRY_HDR = struct.calcsize(DIR_ENTRY_FMT)


def read_sector(data: bytes, sector: int) -> bytes:
    off = sector * SECTOR_SIZE
    return data[off:off + SECTOR_SIZE]


def parse_entry(raw: bytes, off: int = 0):
    flags, name_raw, f1, f2, f3 = struct.unpack_from(DIR_ENTRY_FMT, raw, off)
    name = name_raw.split(b"\x00")[0].decode("utf-8", errors="replace")
    return flags, name, f1, f2, f3


def find_in_dir(data: bytes, dir_sector: int, dir_sectors: int, filename: str):
    """Scan directory block(s) for a file entry. Returns (sector_start, length) or None."""
    sector = dir_sector
    while sector != 0:
        next_continuation = 0
        actual_dir_sectors = dir_sectors
        for s in range(actual_dir_sectors):
            sec = read_sector(data, sector + s)
            for i in range(2):
                entry_off = i * ENTRY_SIZE
                flags, name, f1, f2, f3 = parse_entry(sec, entry_off)
                if s == 0 and i == 0:
                    # Directory start entry: f2=dir_sectors, f3=continuation
                    next_continuation = f3
                    continue
                if flags & FLAG_DIRECTORY:
                    continue  # skip subdirectories
                if flags == 0 and name == filename and f2 > 0:
                    return f1, f2  # sector_start, length
        sector = next_continuation
        if sector == 0:
            break
    return None


def extract(img_path: Path, filename: str, out_path: Path) -> bool:
    data = img_path.read_bytes()
    if len(data) < SECTOR_SIZE:
        print(f"error: image too small", file=sys.stderr)
        return False

    # Parse description block (sector 0)
    sec0 = data[:SECTOR_SIZE]
    identifier, root_directory = struct.unpack_from("<QQ", sec0, 0)
    if identifier != RFS_ID:
        print(f"error: not a RetroFS image (id={identifier:#x})", file=sys.stderr)
        return False

    # Read root directory start to get dir_sectors count
    root_sec = read_sector(data, root_directory)
    _, _, _, dir_sectors, _ = parse_entry(root_sec, 0)

    result = find_in_dir(data, root_directory, dir_sectors, filename)
    if result is None:
        print(f"error: '{filename}' not found in RetroFS root directory", file=sys.stderr)
        return False

    file_sector, file_length = result
    file_start = file_sector * SECTOR_SIZE
    file_end = file_start + file_length
    if file_end > len(data):
        print(f"error: file data out of bounds ({file_end} > {len(data)})", file=sys.stderr)
        return False

    out_path.write_bytes(data[file_start:file_end])
    print(f"extracted '{filename}' ({file_length} bytes) -> {out_path}")
    return True


if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("usage: extract_retrofs_file.py <img> <filename> <output>", file=sys.stderr)
        sys.exit(1)
    ok = extract(Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]))
    sys.exit(0 if ok else 1)
