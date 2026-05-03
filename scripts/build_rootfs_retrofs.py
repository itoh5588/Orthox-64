#!/usr/bin/env python3
import math
import os
import stat
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

SECTOR_SIZE = 512
MAX_NAME = 128
DIR_SECTORS = 64
MAX_DIR_SECTORS = 256
ENTRY_SIZE = SECTOR_SIZE // 2
RFS_FLAG_DIRECTORY = 0x0001
RFS_FLAG_DIR_START = 0x0004
RFS_ID = 0x3153466F72746552
MIN_IMAGE_SECTORS = 4096
DEFAULT_EXTRA_MIB = 64


@dataclass
class Node:
    host_path: Path
    rel_path: str
    is_dir: bool
    mtime: int
    size: int = 0
    children: list["Node"] = field(default_factory=list)
    start_sector: int = 0
    sector_length: int = 0
    continuation_sectors: list[int] = field(default_factory=list)

    @property
    def name(self) -> str:
        return self.host_path.name


def should_skip(name: str) -> bool:
    return name == ".DS_Store" or name.startswith("._")


def build_tree(root: Path, path: Path | None = None) -> Node:
    if path is None:
        path = root
    st = path.stat()
    rel = "" if path == root else str(path.relative_to(root)).replace(os.sep, "/")
    node = Node(
        host_path=path,
        rel_path=rel,
        is_dir=path.is_dir(),
        mtime=int(st.st_mtime),
        size=st.st_size if path.is_file() else 0,
    )
    if node.is_dir:
        entries = []
        for child in sorted(path.iterdir(), key=lambda p: p.name):
            if should_skip(child.name):
                continue
            entries.append(build_tree(root, child))
        node.children = entries
        node.size = 0
    return node


def round_up(value: int, align: int) -> int:
    return (value + align - 1) // align * align


def assign_layout(node: Node, next_sector: int) -> int:
    if node.is_dir:
        block_count = max(1, math.ceil(len(node.children) / (DIR_SECTORS * 2 - 1)))
        node.start_sector = next_sector
        node.sector_length = DIR_SECTORS
        node.continuation_sectors = []
        next_sector += DIR_SECTORS
        for _ in range(1, block_count):
            node.continuation_sectors.append(next_sector)
            next_sector += DIR_SECTORS
        for child in node.children:
            next_sector = assign_layout(child, next_sector)
    else:
        node.start_sector = next_sector
        node.sector_length = max(1, round_up(node.size, SECTOR_SIZE) // SECTOR_SIZE)
        next_sector += node.sector_length
    return next_sector


def copy_name(name: str) -> bytes:
    encoded = name.encode("utf-8")
    if len(encoded) >= MAX_NAME:
        raise RuntimeError(f"name too long for RetroFS: {name}")
    return encoded + b"\0" * (MAX_NAME - len(encoded))


def pack_entry(child: Node) -> bytes:
    flags = RFS_FLAG_DIRECTORY if child.is_dir else 0
    return struct.pack(
        "<I128sQQQqqQ76x",
        flags,
        copy_name(child.name),
        child.start_sector,
        child.size,
        child.sector_length,
        child.mtime,
        child.mtime,
        1,
    )


def write_directory_block(image: bytearray, sector: int, title: str, parent_sector: int, continuation: int, children: list[Node]) -> None:
    base = sector * SECTOR_SIZE
    image[base:base + ENTRY_SIZE] = struct.pack(
        "<I128sQQQ100x",
        RFS_FLAG_DIR_START,
        copy_name(title),
        parent_sector,
        DIR_SECTORS,
        continuation,
    )
    for idx, child in enumerate(children, start=1):
        off = base + idx * ENTRY_SIZE
        image[off:off + ENTRY_SIZE] = pack_entry(child)


def write_directory(image: bytearray, node: Node, parent_sector: int) -> None:
    sectors = [node.start_sector] + node.continuation_sectors
    slots = DIR_SECTORS * 2 - 1
    for block_index, sector in enumerate(sectors):
        start = block_index * slots
        end = start + slots
        continuation = sectors[block_index + 1] if block_index + 1 < len(sectors) else 0
        write_directory_block(
            image,
            sector,
            node.name or "/",
            parent_sector,
            continuation,
            node.children[start:end],
        )
    for child in node.children:
        if child.is_dir:
            write_directory(image, child, node.start_sector)


def write_file(image: bytearray, node: Node) -> None:
    with node.host_path.open("rb") as f:
        data = f.read()
    base = node.start_sector * SECTOR_SIZE
    image[base:base + len(data)] = data


def write_files(image: bytearray, node: Node) -> None:
    if node.is_dir:
        for child in node.children:
            write_files(image, child)
    else:
        write_file(image, node)


def iter_nodes(node: Node):
    yield node
    for child in node.children:
        yield from iter_nodes(child)


def build_bitmap(total_sectors: int, used_until: int, map_start: int) -> bytes:
    map_bytes = bytearray((total_sectors + 7) // 8)
    for sector in range(used_until):
        map_bytes[sector // 8] |= 1 << (sector % 8)
    for sector in range(map_start, total_sectors):
        map_bytes[sector // 8] |= 1 << (sector % 8)
    return bytes(map_bytes)


def choose_total_sectors(used_without_map: int, extra_sectors: int) -> tuple[int, int]:
    total = max(MIN_IMAGE_SECTORS, used_without_map + extra_sectors + 1)
    while True:
        map_sectors = math.ceil(total / 8 / SECTOR_SIZE)
        new_total = max(MIN_IMAGE_SECTORS, used_without_map + extra_sectors + map_sectors)
        if new_total == total:
            return total, map_sectors
        total = new_total


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: build_rootfs_retrofs.py ROOT_DIR OUT_IMG", file=sys.stderr)
        return 1

    root = Path(sys.argv[1])
    out_img = Path(sys.argv[2])
    extra_mib = int(os.environ.get("EXTRA_MIB", str(DEFAULT_EXTRA_MIB)))

    if not root.is_dir():
        print(f"missing root directory: {root}", file=sys.stderr)
        return 1

    tree = build_tree(root)
    used_without_map = assign_layout(tree, 1)
    extra_sectors = max(0, extra_mib) * 1024 * 1024 // SECTOR_SIZE
    total_sectors, map_sectors = choose_total_sectors(used_without_map, extra_sectors)
    map_start = total_sectors - map_sectors

    if used_without_map >= map_start:
        print("RetroFS image layout overflow", file=sys.stderr)
        return 1

    image = bytearray(total_sectors * SECTOR_SIZE)
    write_directory(image, tree, 0)
    write_files(image, tree)

    now = int(time.time())
    desc = struct.pack(
        "<QQQQQQq",
        RFS_ID,
        tree.start_sector,
        map_start,
        map_sectors,
        0,
        1,
        now,
    )
    image[0:len(desc)] = desc

    bitmap = build_bitmap(total_sectors, used_without_map, map_start)
    image[map_start * SECTOR_SIZE: map_start * SECTOR_SIZE + len(bitmap)] = bitmap

    out_img.write_bytes(image)
    used_nodes = sum(1 for _ in iter_nodes(tree))
    print(
        f"built RetroFS image {out_img} sectors={total_sectors} used={used_without_map} "
        f"map_start={map_start} nodes={used_nodes}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
