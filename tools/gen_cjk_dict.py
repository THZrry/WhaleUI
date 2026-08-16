#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate the CJK dictionary for word navigation as a binary resource.

Reads jieba's dict.txt (installed via `pip install jieba`; words listed by
descending frequency) and writes res/whaleui_dict.bin:

    magic   "WUID"                  (4 bytes)
    version u32 LE                  (1)
    group_count u32 LE              (number of distinct head characters)
    per group:
        u8   head_len, head bytes   (UTF-8, usually 3)
        u16  tail_count
        u32  tails_blob_len
        tails_blob: tail_count × (u8 len + bytes)  (word minus its head)

Grouping by head char stores each head once; only the tails repeat, which
keeps the file compact (~2 MB for the full 330k-word lexicon). The loader
checks magic + version and falls back to a tiny built-in list when the file
is missing or damaged.

Full and lite builds share the same file (full lexicon); minimal builds do
no segmentation at all. Run from the repo root:
    python tools/gen_cjk_dict.py
"""
import os
import struct
import sys


def load_words():
    import jieba
    dict_path = os.path.join(os.path.dirname(jieba.__file__), "dict.txt")
    scored = []
    with open(dict_path, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 2:
                continue
            w = parts[0]
            if not (2 <= len(w) <= 4):
                continue
            if not all("\u4e00" <= c <= "\u9fff" for c in w):
                continue  # pure hanzi only
            try:
                freq = int(parts[1])
            except ValueError:
                freq = 0
            scored.append((freq, w))
    scored.sort(key=lambda kv: -kv[0])  # descending frequency
    return [w for _, w in scored]


def gen_bin(path, words, version):
    groups = {}
    for w in words:
        groups.setdefault(w[0], []).append(w[1:])
    items = sorted(groups.items(), key=lambda kv: kv[0].encode("utf-8"))
    out = bytearray(b"WUID")
    out += struct.pack("<II", version, len(items))
    for head, tails in items:
        hb = head.encode("utf-8")
        if len(hb) > 255:
            raise ValueError("head too long: %s" % head)
        blob = bytearray()
        for t in tails:
            tb = t.encode("utf-8")
            blob += bytes([len(tb)]) + tb
        if len(tails) > 0xFFFF or len(blob) > 0xFFFFFFFF:
            raise ValueError("group too big: %s" % head)
        out += bytes([len(hb)]) + hb
        out += struct.pack("<HI", len(tails), len(blob))
        out += blob
    with open(path, "wb") as f:
        f.write(bytes(out))
    print("wrote %s (%d words, %d groups, %.2f MB)" %
          (path, len(words), len(items), len(out) / 1048576.0))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    words = load_words()
    res = os.path.join(root, "res")
    os.makedirs(res, exist_ok=True)
    gen_bin(os.path.join(res, "whaleui_dict.bin"), words, 1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
