#!/usr/bin/env python3
"""
Parse OsFinalProject custom filesystem image (data.fs).

Matches C++ layout in:
  - src/server/filesystem/superblock.hpp
  - src/server/filesystem/inode.hpp
  - src/server/filesystem/vfs.cpp (DirEntry, resolvePath, readFile)

Usage examples:
  python3 tools/parse_data_fs.py info /path/to/data.fs
  python3 tools/parse_data_fs.py ls /path/to/data.fs /
  python3 tools/parse_data_fs.py cat /path/to/data.fs /a/b.txt
  python3 tools/parse_data_fs.py bitmap /path/to/data.fs
"""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from typing import Iterable, List, Tuple


FS_MAGIC = 0x20251205

# C++ Inode layout (typical on clang/macOS):
#   uint32 id (4)
#   bool isDirectory (1)
#   padding (3)
#   uint32 size (4)
#   uint32 directBlocks[8] (32)
# Total: 44
INODE_SIZE = 44
INODE_STRUCT = struct.Struct("<I?3xI8I")

# C++ DirEntry layout:
#   uint32 inodeId (4)
#   char name[60] (60)
# Total: 64
DIRENTRY_SIZE = 64
DIRENTRY_STRUCT = struct.Struct("<I60s")


@dataclass(frozen=True)
class SuperBlock:
    magic: int
    blockSize: int
    totalBlocks: int
    inodeTableStart: int
    inodeTableBlocks: int
    inodeCount: int
    freeBitmapStart: int
    freeBitmapBlocks: int
    dataBlockStart: int
    dataBlockCount: int
    rootInodeId: int


@dataclass
class Inode:
    id: int
    is_dir: bool
    size: int
    direct: List[int]


def read_exact(f, n: int) -> bytes:
    data = f.read(n)
    if len(data) != n:
        raise ValueError(f"unexpected EOF: need {n} bytes, got {len(data)} bytes")
    return data


def read_superblock(f) -> SuperBlock:
    raw = read_exact(f, 44)  # 11 * uint32
    vals = struct.unpack("<11I", raw)
    sb = SuperBlock(*vals)
    if sb.magic != FS_MAGIC:
        raise ValueError(f"bad magic: {hex(sb.magic)} (expected {hex(FS_MAGIC)})")
    if sb.blockSize <= 0 or sb.totalBlocks <= 0:
        raise ValueError("invalid superblock: blockSize/totalBlocks")
    return sb


def file_offset_of_block(sb: SuperBlock, block_id: int) -> int:
    return block_id * sb.blockSize


def read_block(f, sb: SuperBlock, block_id: int) -> bytes:
    if block_id < 0 or block_id >= sb.totalBlocks:
        raise ValueError(f"blockId out of range: {block_id}")
    f.seek(file_offset_of_block(sb, block_id))
    return read_exact(f, sb.blockSize)


def inode_offset(sb: SuperBlock, inode_id: int) -> int:
    inodes_per_block = sb.blockSize // INODE_SIZE
    if inodes_per_block <= 0:
        raise ValueError("invalid layout: inodes_per_block <= 0")
    if inode_id < 0 or inode_id >= sb.inodeCount:
        raise ValueError(f"inodeId out of range: {inode_id}")
    block_index = inode_id // inodes_per_block
    index_in_block = inode_id % inodes_per_block
    if block_index >= sb.inodeTableBlocks:
        raise ValueError("inode table block index out of range (superblock mismatch?)")
    block_id = sb.inodeTableStart + block_index
    return file_offset_of_block(sb, block_id) + index_in_block * INODE_SIZE


def read_inode(f, sb: SuperBlock, inode_id: int) -> Inode:
    f.seek(inode_offset(sb, inode_id))
    raw = read_exact(f, INODE_SIZE)
    iid, isdir, size, *direct = INODE_STRUCT.unpack(raw)
    return Inode(id=iid, is_dir=bool(isdir), size=int(size), direct=[int(x) for x in direct])


def split_path(path: str) -> List[str]:
    # mimic Vfs::splitPath: just split on '/', ignoring empty components
    comps: List[str] = []
    cur = []
    for ch in path:
        if ch == "/":
            if cur:
                comps.append("".join(cur))
                cur = []
        else:
            cur.append(ch)
    if cur:
        comps.append("".join(cur))
    return comps


def parse_dir_block(block: bytes) -> List[Tuple[int, str]]:
    out: List[Tuple[int, str]] = []
    for i in range(0, len(block), DIRENTRY_SIZE):
        chunk = block[i : i + DIRENTRY_SIZE]
        if len(chunk) != DIRENTRY_SIZE:
            break
        inode_id, name_raw = DIRENTRY_STRUCT.unpack(chunk)
        if inode_id == 0:
            continue
        name = name_raw.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
        out.append((int(inode_id), name))
    return out


def resolve_path(f, sb: SuperBlock, path: str) -> int:
    if path == "" or path == "/":
        return sb.rootInodeId
    comps = split_path(path)
    if not comps:
        raise ValueError("invalid path")

    current = sb.rootInodeId
    for name in comps:
        ino = read_inode(f, sb, current)
        if not ino.is_dir:
            raise ValueError(f"not a directory while resolving: inode {current}")
        if ino.direct[0] == 0:
            raise ValueError(f"directory inode {current} has no data block")
        entries = parse_dir_block(read_block(f, sb, ino.direct[0]))
        nxt = None
        for child_id, child_name in entries:
            if child_name == name:
                nxt = child_id
                break
        if nxt is None:
            raise ValueError(f"path component not found: {name}")
        current = nxt
    return current


def list_dir(f, sb: SuperBlock, path: str) -> List[str]:
    inode_id = resolve_path(f, sb, path)
    ino = read_inode(f, sb, inode_id)
    if not ino.is_dir:
        raise ValueError("not a directory")
    if ino.direct[0] == 0:
        return []
    entries = parse_dir_block(read_block(f, sb, ino.direct[0]))
    out: List[str] = []
    for child_id, child_name in entries:
        child = read_inode(f, sb, child_id)
        out.append(child_name + ("/" if child.is_dir else ""))
    return out


def read_file_content(f, sb: SuperBlock, path: str) -> bytes:
    inode_id = resolve_path(f, sb, path)
    ino = read_inode(f, sb, inode_id)
    if ino.is_dir:
        raise ValueError("path is a directory")

    remaining = ino.size
    chunks: List[bytes] = []
    for block_id in ino.direct:
        if remaining <= 0:
            break
        if block_id == 0:
            break
        blk = read_block(f, sb, block_id)
        take = min(remaining, sb.blockSize)
        chunks.append(blk[:take])
        remaining -= take
    if remaining != 0:
        raise ValueError("file is truncated or inode pointers are incomplete")
    return b"".join(chunks)


def bitmap_stats(f, sb: SuperBlock) -> Tuple[int, int, int]:
    # only considers data blocks [dataBlockStart, dataBlockStart+dataBlockCount)
    used = 0
    remaining = sb.dataBlockCount
    bit_index = 0
    for b in range(sb.freeBitmapBlocks):
        if remaining <= 0:
            break
        bmp = read_block(f, sb, sb.freeBitmapStart + b)
        bits_in_this_block = min(len(bmp) * 8, remaining)
        for _ in range(bits_in_this_block):
            byte = bmp[bit_index // 8]
            if (byte >> (bit_index % 8)) & 1:
                used += 1
            bit_index += 1
        remaining -= bits_in_this_block
    total = sb.dataBlockCount
    return total, used, total - used


def main(argv: Iterable[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_info = sub.add_parser("info")
    p_info.add_argument("image")

    p_ls = sub.add_parser("ls")
    p_ls.add_argument("image")
    p_ls.add_argument("path", nargs="?", default="/")

    p_cat = sub.add_parser("cat")
    p_cat.add_argument("image")
    p_cat.add_argument("path")
    p_cat.add_argument("--raw", action="store_true", help="output raw bytes (may break terminal)")

    p_bm = sub.add_parser("bitmap")
    p_bm.add_argument("image")

    args = ap.parse_args(list(argv) if argv is not None else None)

    with open(args.image, "rb") as f:
        sb = read_superblock(f)

        if args.cmd == "info":
            print("magic", hex(sb.magic))
            print("blockSize", sb.blockSize)
            print("totalBlocks", sb.totalBlocks)
            print("inodeTableStart", sb.inodeTableStart)
            print("inodeTableBlocks", sb.inodeTableBlocks)
            print("inodeCount", sb.inodeCount)
            print("freeBitmapStart", sb.freeBitmapStart)
            print("freeBitmapBlocks", sb.freeBitmapBlocks)
            print("dataBlockStart", sb.dataBlockStart)
            print("dataBlockCount", sb.dataBlockCount)
            print("rootInodeId", sb.rootInodeId)
            root = read_inode(f, sb, sb.rootInodeId)
            print("root inode", root.id, "isDirectory", int(root.is_dir), "size", root.size, "direct0", root.direct[0])
            return 0

        if args.cmd == "ls":
            for name in list_dir(f, sb, args.path):
                print(name)
            return 0

        if args.cmd == "cat":
            data = read_file_content(f, sb, args.path)
            if args.raw:
                import sys

                sys.stdout.buffer.write(data)
            else:
                # best-effort: print as UTF-8
                print(data.decode("utf-8", errors="replace"), end="")
            return 0

        if args.cmd == "bitmap":
            total, used, free = bitmap_stats(f, sb)
            print("dataBlocks", total)
            print("used", used)
            print("free", free)
            return 0

    return 0


if __name__ == "__main__":
    raise SystemExit(main())


