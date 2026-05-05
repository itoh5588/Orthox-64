#!/usr/bin/env python3
"""
build_rootfs_xv6fs.py
xv6fs フォーマットの rootfs イメージを生成する。
Orthox-64 向け拡張:
  FSSIZE=327680 blocks (320 MB), NINODES=8192,
  triple indirect ブロック対応 (最大 ~16 GB/file)
"""
import os
import struct
import sys
from pathlib import Path

# ──────────────────────────────────────────────
# FS 定数 (Orthox-64 拡張版)
# ──────────────────────────────────────────────
BSIZE      = 1024
FSSIZE     = 327680       # 320 MB in 1 KB blocks
NINODES    = 8192
LOGBLOCKS  = 126          # log data-blocks (excluding header block)

NDIRECT    = 9            # 直接ブロック数
NINDIRECT  = BSIZE // 4   # 1 段間接: 256
NDINDIRECT = NINDIRECT * NINDIRECT              # 2 段間接: 65536
NTINDIRECT = NINDIRECT * NINDIRECT * NINDIRECT  # 3 段間接: 16777216
MAXFILE    = NDIRECT + NINDIRECT + NDINDIRECT + NTINDIRECT

FSMAGIC  = 0x10203040
ROOTINO  = 1

T_DIR    = 1
T_FILE   = 2
T_DEVICE = 3
DIRSIZ   = 62   # dirent = inum(2) + name(62) = 64 bytes; BSIZE/64 = 16 entries/block

# dinode on-disk layout:
#   short type(2) + short major(2) + short minor(2) + short nlink(2)
#   + uint size(4) + uint addrs[NDIRECT+3](4×12) = 60 bytes
#   addrs[0..8]=direct(9), [9]=indirect, [10]=dindirect, [11]=tindirect
DINODE_ADDRS = NDIRECT + 3            # 12 (9+3)
DINODE_SIZE  = 2 + 2 + 2 + 2 + 4 + 4 * DINODE_ADDRS  # 60
DINODE_FMT   = f"<hhhhI{DINODE_ADDRS}I"

IPB = BSIZE // DINODE_SIZE            # 17 inodes per block

# dirent on-disk layout: ushort inum(2) + char name[62] = 64 bytes
DIRSIZ_BYTES = DIRSIZ
DIRENT_FMT   = f"<H{DIRSIZ}s"

# ──────────────────────────────────────────────
# FS layout (computed)
# ──────────────────────────────────────────────
nlog         = LOGBLOCKS + 1               # 127 (1 header + 126 data)
ninodeblocks = NINODES // IPB + 1
nbitmap      = FSSIZE // (BSIZE * 8) + 1
nmeta        = 2 + nlog + ninodeblocks + nbitmap
nblocks      = FSSIZE - nmeta

logstart     = 2
inodestart   = 2 + nlog
bmapstart    = 2 + nlog + ninodeblocks

# ──────────────────────────────────────────────
# グローバル状態
# ──────────────────────────────────────────────
freeinode: int      = 1
freeblock: int      = nmeta
image: bytearray    = bytearray(FSSIZE * BSIZE)


# ──────────────────────────────────────────────
# ブロック I/O
# ──────────────────────────────────────────────
def wsect(sec: int, data: bytes) -> None:
    assert 0 <= sec < FSSIZE, f"wsect out of range: {sec}"
    assert len(data) == BSIZE, f"wsect wrong size: {len(data)}"
    base = sec * BSIZE
    image[base:base + BSIZE] = data


def rsect(sec: int) -> bytearray:
    assert 0 <= sec < FSSIZE, f"rsect out of range: {sec}"
    base = sec * BSIZE
    return bytearray(image[base:base + BSIZE])


# ──────────────────────────────────────────────
# inode I/O
# ──────────────────────────────────────────────
def _iblock(inum: int) -> int:
    return inum // IPB + inodestart


def winode(inum: int, din: dict) -> None:
    bn  = _iblock(inum)
    buf = rsect(bn)
    off = (inum % IPB) * DINODE_SIZE
    packed = struct.pack(
        DINODE_FMT,
        din['type'], din['major'], din['minor'], din['nlink'],
        din['size'],
        *din['addrs'],
    )
    buf[off:off + DINODE_SIZE] = packed
    wsect(bn, bytes(buf))


def rinode(inum: int) -> dict:
    bn   = _iblock(inum)
    buf  = rsect(bn)
    off  = (inum % IPB) * DINODE_SIZE
    flds = struct.unpack(DINODE_FMT, buf[off:off + DINODE_SIZE])
    return {
        'type':  flds[0],
        'major': flds[1],
        'minor': flds[2],
        'nlink': flds[3],
        'size':  flds[4],
        'addrs': list(flds[5:]),
    }


def ialloc(ftype: int) -> int:
    global freeinode
    inum = freeinode
    freeinode += 1
    assert inum < NINODES, f"out of inodes: {inum} (max {NINODES})"
    din = {'type': ftype, 'major': 0, 'minor': 0, 'nlink': 1,
           'size': 0, 'addrs': [0] * DINODE_ADDRS}
    winode(inum, din)
    return inum


# ──────────────────────────────────────────────
# ブロック割り当て補助
# ──────────────────────────────────────────────
def _alloc_block() -> int:
    global freeblock
    b = freeblock
    freeblock += 1
    assert b < FSSIZE, f"out of data blocks: {b} (FSSIZE={FSSIZE})"
    return b


def _read_uints(bno: int) -> list:
    return list(struct.unpack(f"<{BSIZE // 4}I", rsect(bno)))


def _write_uints(bno: int, arr: list) -> None:
    wsect(bno, struct.pack(f"<{BSIZE // 4}I", *arr))


# ──────────────────────────────────────────────
# iappend — double indirect 対応
# ──────────────────────────────────────────────
def iappend(inum: int, data: bytes | bytearray) -> None:
    din    = rinode(inum)
    addrs  = din['addrs']
    off    = din['size']
    mv     = memoryview(bytes(data))
    remain = len(mv)

    while remain > 0:
        fbn     = off // BSIZE
        blkoff  = off % BSIZE
        n1      = min(remain, BSIZE - blkoff)

        assert fbn < MAXFILE, f"iappend: file too large (fbn={fbn}, MAXFILE={MAXFILE})"

        if fbn < NDIRECT:
            if addrs[fbn] == 0:
                addrs[fbn] = _alloc_block()
            x = addrs[fbn]

        elif fbn < NDIRECT + NINDIRECT:
            # 1 段間接 (addrs[NDIRECT])
            if addrs[NDIRECT] == 0:
                addrs[NDIRECT] = _alloc_block()
                _write_uints(addrs[NDIRECT], [0] * (BSIZE // 4))
            ind = _read_uints(addrs[NDIRECT])
            idx = fbn - NDIRECT
            if ind[idx] == 0:
                ind[idx] = _alloc_block()
                _write_uints(addrs[NDIRECT], ind)
            x = ind[idx]

        elif fbn < NDIRECT + NINDIRECT + NDINDIRECT:
            # 2 段間接 (addrs[NDIRECT+1])
            if addrs[NDIRECT + 1] == 0:
                addrs[NDIRECT + 1] = _alloc_block()
                _write_uints(addrs[NDIRECT + 1], [0] * (BSIZE // 4))
            dind = _read_uints(addrs[NDIRECT + 1])
            rel  = fbn - NDIRECT - NINDIRECT
            i2   = rel // NINDIRECT
            i1   = rel % NINDIRECT
            if dind[i2] == 0:
                dind[i2] = _alloc_block()
                _write_uints(addrs[NDIRECT + 1], dind)
                _write_uints(dind[i2], [0] * (BSIZE // 4))
            ind = _read_uints(dind[i2])
            if ind[i1] == 0:
                ind[i1] = _alloc_block()
                _write_uints(dind[i2], ind)
            x = ind[i1]

        else:
            # 3 段間接 (addrs[NDIRECT+2])
            if addrs[NDIRECT + 2] == 0:
                addrs[NDIRECT + 2] = _alloc_block()
                _write_uints(addrs[NDIRECT + 2], [0] * (BSIZE // 4))
            rel = fbn - NDIRECT - NINDIRECT - NDINDIRECT
            i3  = rel // NDINDIRECT
            r2  = rel % NDINDIRECT
            i2  = r2 // NINDIRECT
            i1  = r2 % NINDIRECT
            # L1 テーブル
            tind = _read_uints(addrs[NDIRECT + 2])
            if tind[i3] == 0:
                tind[i3] = _alloc_block()
                _write_uints(addrs[NDIRECT + 2], tind)
                _write_uints(tind[i3], [0] * (BSIZE // 4))
            # L2 テーブル
            dind = _read_uints(tind[i3])
            if dind[i2] == 0:
                dind[i2] = _alloc_block()
                _write_uints(tind[i3], dind)
                _write_uints(dind[i2], [0] * (BSIZE // 4))
            # L3 テーブル
            ind = _read_uints(dind[i2])
            if ind[i1] == 0:
                ind[i1] = _alloc_block()
                _write_uints(dind[i2], ind)
            x = ind[i1]

        buf = rsect(x)
        buf[blkoff:blkoff + n1] = mv[:n1]
        wsect(x, bytes(buf))

        mv     = mv[n1:]
        remain -= n1
        off    += n1

    din['size']  = off
    din['addrs'] = addrs
    winode(inum, din)


# ──────────────────────────────────────────────
# ディレクトリ操作
# ──────────────────────────────────────────────
def _dirent(inum: int, name: str | bytes) -> bytes:
    if isinstance(name, str):
        nb = name.encode('utf-8', errors='replace')
    else:
        nb = name
    nb = nb[:DIRSIZ].ljust(DIRSIZ, b'\x00')
    return struct.pack(DIRENT_FMT, inum, nb)


def add_dirent(parent: int, name: str, child: int) -> None:
    iappend(parent, _dirent(child, name))


def mkdirnode(parent: int | None, name: str | None) -> int:
    inum = ialloc(T_DIR)
    iappend(inum, _dirent(inum, "."))
    iappend(inum, _dirent(parent if parent is not None else inum, ".."))
    if parent is not None and name is not None:
        add_dirent(parent, name, inum)
    return inum


def should_skip(name: str) -> bool:
    return name == '.DS_Store' or name.startswith('._')


# ──────────────────────────────────────────────
# rootfs ツリーの書き込み
# ──────────────────────────────────────────────
def populate(host_path: Path, dir_inum: int, depth: int = 0) -> None:
    try:
        entries = sorted(host_path.iterdir(), key=lambda p: (p.is_dir(), p.name))
    except PermissionError as e:
        print(f"\nSKIP(perm) {host_path}: {e}", file=sys.stderr)
        return

    total = len(entries)
    done  = 0

    for entry in entries:
        done += 1
        name = entry.name
        if should_skip(name):
            continue

        name_b = name.encode('utf-8', errors='replace')
        if len(name_b) > DIRSIZ:
            truncated = name_b[:DIRSIZ].decode('latin-1')
            print(f"\nWARN: name truncated {name!r} -> {truncated!r}", file=sys.stderr)
            name = truncated

        if entry.is_symlink():
            try:
                target = os.readlink(entry).encode()
                child  = ialloc(T_FILE)
                add_dirent(dir_inum, name, child)
                iappend(child, target)
            except OSError as e:
                print(f"\nSKIP symlink {entry}: {e}", file=sys.stderr)

        elif entry.is_dir():
            child = mkdirnode(dir_inum, name)
            populate(entry, child, depth + 1)

        else:
            try:
                fsize = entry.stat().st_size
                child = ialloc(T_FILE)
                add_dirent(dir_inum, name, child)
                with entry.open('rb') as f:
                    while True:
                        chunk = f.read(512 * 1024)
                        if not chunk:
                            break
                        iappend(child, chunk)
                if depth == 0 or fsize >= 1 << 20:
                    print(f"\n  {'  ' * depth}{name}  ({fsize:,} B)", end='')
            except (PermissionError, OSError) as e:
                print(f"\nSKIP {entry}: {e}", file=sys.stderr)

        if depth == 0:
            pct = done * 100 // max(total, 1)
            print(f"\r  [{done:4d}/{total}] {pct:3d}%  {name[:32]:<32}", end='', flush=True)

    if depth == 0:
        print()


# ──────────────────────────────────────────────
# ビットマップ書き込み
# ──────────────────────────────────────────────
def write_bitmap() -> None:
    print(f"bitmap: {freeblock} blocks used, {nbitmap} bitmap block(s) at {bmapstart}")
    for i in range(nbitmap):
        buf  = bytearray(BSIZE)
        base = i * BSIZE * 8
        lim  = max(0, min(freeblock - base, BSIZE * 8))
        for b in range(lim):
            buf[b // 8] |= 1 << (b % 8)
        wsect(bmapstart + i, bytes(buf))


# ──────────────────────────────────────────────
# xv6fs ファイル抽出 (読み取り専用)
# ──────────────────────────────────────────────
def _get_data_block(addrs: list, fbn: int) -> int:
    """ファイルブロック番号 fbn に対応するディスクブロック番号を返す (未割り当て=0)。"""
    if fbn < NDIRECT:
        return addrs[fbn]
    fbn -= NDIRECT
    if fbn < NINDIRECT:
        if addrs[NDIRECT] == 0:
            return 0
        return _read_uints(addrs[NDIRECT])[fbn]
    fbn -= NINDIRECT
    if fbn < NDINDIRECT:
        if addrs[NDIRECT + 1] == 0:
            return 0
        dind = _read_uints(addrs[NDIRECT + 1])
        i2, i1 = fbn // NINDIRECT, fbn % NINDIRECT
        if dind[i2] == 0:
            return 0
        return _read_uints(dind[i2])[i1]
    fbn -= NDINDIRECT
    if fbn < NTINDIRECT:
        if addrs[NDIRECT + 2] == 0:
            return 0
        tind = _read_uints(addrs[NDIRECT + 2])
        i3  = fbn // NDINDIRECT
        r2  = fbn % NDINDIRECT
        i2, i1 = r2 // NINDIRECT, r2 % NINDIRECT
        if tind[i3] == 0:
            return 0
        dind = _read_uints(tind[i3])
        if dind[i2] == 0:
            return 0
        return _read_uints(dind[i2])[i1]
    return 0


def _read_file_data(din: dict) -> bytes:
    """inode の全データバイトを読み取って返す。"""
    size   = din['size']
    addrs  = din['addrs']
    result = bytearray()
    fbn    = 0
    while len(result) < size:
        blk = _get_data_block(addrs, fbn)
        if blk == 0:
            break
        to_read = min(BSIZE, size - len(result))
        result.extend(rsect(blk)[:to_read])
        fbn += 1
    return bytes(result)


def _find_dirent(dir_inum: int, din: dict, name: str) -> int | None:
    """ディレクトリ内の name を検索し、子 inum を返す。見つからなければ None。"""
    data = _read_file_data(din)
    entry_size = 2 + DIRSIZ
    for i in range(0, len(data), entry_size):
        raw = data[i:i + entry_size]
        if len(raw) < entry_size:
            break
        child_inum, name_b = struct.unpack(DIRENT_FMT, raw)
        if child_inum == 0:
            continue
        if name_b.rstrip(b'\x00').decode('utf-8', errors='replace') == name:
            return child_inum
    return None


def extract_file_from_image(img_path: str, fs_path: str, out_path: str) -> int:
    """xv6fs イメージから fs_path のファイルを out_path に書き出す。"""
    global image, inodestart, bmapstart, logstart

    raw = Path(img_path).read_bytes()
    image = bytearray(raw)

    sb_fields = struct.unpack("<IIIIIIII", bytes(image[BSIZE:BSIZE + 32]))
    magic, _, _, _, _, logstart_sb, inodestart_sb, bmapstart_sb = sb_fields
    if magic != FSMAGIC:
        print(f"ERROR: bad magic 0x{magic:08x} (expected 0x{FSMAGIC:08x})", file=sys.stderr)
        return 1

    inodestart = inodestart_sb
    bmapstart  = bmapstart_sb
    logstart   = logstart_sb

    parts = [p for p in fs_path.strip('/').split('/') if p]
    inum  = ROOTINO

    for part in parts:
        din = rinode(inum)
        if din['type'] != T_DIR:
            print(f"ERROR: inum={inum} is not a directory (looking for '{part}')", file=sys.stderr)
            return 1
        child = _find_dirent(inum, din, part)
        if child is None:
            print(f"ERROR: '{part}' not found", file=sys.stderr)
            return 1
        inum = child

    din = rinode(inum)
    if din['type'] != T_FILE:
        print(f"ERROR: {fs_path!r} is not a regular file (type={din['type']})", file=sys.stderr)
        return 1

    data = _read_file_data(din)
    Path(out_path).write_bytes(data)
    print(f"Extracted {len(data):,} bytes: {fs_path} -> {out_path}")
    return 0


# ──────────────────────────────────────────────
# main
# ──────────────────────────────────────────────
def main() -> int:
    if len(sys.argv) >= 2 and sys.argv[1] == '--extract':
        if len(sys.argv) != 5:
            print("usage: build_rootfs_xv6fs.py --extract FS_PATH OUT_FILE IMG_FILE", file=sys.stderr)
            return 1
        return extract_file_from_image(sys.argv[4], sys.argv[2], sys.argv[3])

    if len(sys.argv) < 3:
        print("usage: build_rootfs_xv6fs.py ROOT_DIR OUT_IMG", file=sys.stderr)
        print("       build_rootfs_xv6fs.py --extract FS_PATH OUT_FILE IMG_FILE", file=sys.stderr)
        return 1

    root_dir = Path(sys.argv[1])
    out_img  = Path(sys.argv[2])

    if not root_dir.is_dir():
        print(f"not a directory: {root_dir}", file=sys.stderr)
        return 1

    # superblock (block 1)
    sb_buf = bytearray(BSIZE)
    sb_buf[:32] = struct.pack(
        "<IIIIIIII",
        FSMAGIC, FSSIZE, nblocks, NINODES,
        nlog, logstart, inodestart, bmapstart,
    )
    wsect(1, bytes(sb_buf))

    print("xv6fs image parameters:")
    print(f"  FSSIZE={FSSIZE} blocks = {FSSIZE * BSIZE // 1024 // 1024} MB")
    print(f"  nmeta={nmeta}  log={nlog}@{logstart}  inode={ninodeblocks}@{inodestart}  bitmap={nbitmap}@{bmapstart}")
    print(f"  nblocks={nblocks}  NINODES={NINODES}  IPB={IPB}  DINODE_SIZE={DINODE_SIZE}")
    print(f"  MAXFILE={MAXFILE} blocks = {MAXFILE * BSIZE // 1024 // 1024} MB/file")

    # ルートディレクトリ (inum=1=ROOTINO)
    rootino = ialloc(T_DIR)
    assert rootino == ROOTINO, f"rootino={rootino} expected {ROOTINO}"
    iappend(rootino, _dirent(rootino, "."))
    iappend(rootino, _dirent(rootino, ".."))

    # rootfs ツリー書き込み
    print(f"\npopulating from {root_dir} ...")
    populate(root_dir, rootino)

    # ルートディレクトリサイズを BSIZE に align (xv6 mkfs 互換)
    din      = rinode(rootino)
    din['size'] = ((din['size'] + BSIZE - 1) // BSIZE) * BSIZE
    winode(rootino, din)

    write_bitmap()

    out_img.write_bytes(image)

    used_mb = freeblock * BSIZE // 1024 // 1024
    print(f"\nimage written: {out_img}  ({FSSIZE * BSIZE // 1024 // 1024} MB total)")
    print(f"  inodes used : {freeinode - 1} / {NINODES}")
    print(f"  blocks used : {freeblock} / {FSSIZE}  ({used_mb} MB)")

    magic = struct.unpack("<I", bytes(image[BSIZE:BSIZE + 4]))[0]
    if magic == FSMAGIC:
        print(f"  superblock magic: OK  (0x{magic:08x})")
        return 0
    else:
        print(f"  ERROR: bad magic 0x{magic:08x}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
