"""Adds the equipment pictures (pictures.py) to the game's image bank, NSW/LIVECONTENT/LIVE.TEX.

On Switch the game draws item pictures (texture_name in InventoryData.xml) from this TX12NSW bank, not from the loose
CROSS/LIVE/*.WEBP files, so a new picture only shows once it is an entry of the bank. Layout (worked out from the file):

  header 0x30   'TX12NSW\\0', u32 0xc, u32 +0x0c sum of the stored sizes rounded up to 16, u32 +0x10 sum of the
                unpacked sizes, u32 +0x14 entry count, u32 +0x18 gap between the table and the data, zeros
  table         0x140 bytes per entry from 0x30: name[64]; +0x40 hash (8 bytes, then 8 bytes the same in every entry);
                +0x48 the hash of its layout (below); +0x50 size, u16 w, u16 h, depth, layers, u32 mips; +0xc0 u32
                unpacked, u32 stored; +0xd0 1 when the data is an LZ4 block, 0 when stored as is; +0x124 u32 offset
  layouts       right after the table, filling the gap: u32 count, u32 size of this block (= the gap), 8 zero bytes,
                then 0x40 per layout (hash, size, w, h, ...). The game's loader (main+0x5cea40) registers these and
                looks up every entry's +0x48 hash among them; a missing one crashes it at startup.
  data          one after the other, each 16-aligned, from table end + gap (4 KiB aligned)
  trailer       32 zero bytes per entry

A picture is a 512x512 BC7 texture, one mip, Tegra block-linear: the same as the shipped ritual pictures, whose entry
(Ritual_Gum) is the template. The original bank is kept once as LIVE.TEX.orig; install always starts from it.
Needs numpy.
"""
import hashlib
import shutil
import struct
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / 'render'))
import bc7  # noqa: E402
from swap import swap_in  # noqa: E402   (HERE is on the path: equipment.py and pictures.py put it there)

ENTRY = 0x140
TEMPLATE = 'ritual_gum'
RAW_FLAGS = bytes([0, 0, 1, 0])     # +0xd0 of the 19 shipped entries stored without LZ4; LZ4 ones have 1 at +0xd0
SIZE = 512


def bank_path(game):
    return game / 'game' / 'romfs' / 'NSW' / 'LIVECONTENT' / 'LIVE.TEX'


# ---- Tegra block-linear (the inverse of render/tx12.py deswizzle) -----------------------------------------------------

def _addresses(width_bytes, rows, bh):
    gobs_x = (width_bytes + 63) // 64
    y = np.arange(rows)[:, None]
    x = np.arange(width_bytes)[None, :]
    gob = (y // (8 * bh)) * (gobs_x * bh * 512) + (x // 64) * (bh * 512) + ((y % (8 * bh)) // 8) * 512
    xi, yi = x % 64, y % 8
    return gob + (xi // 32) * 256 + (yi // 2) * 64 + ((xi % 32) // 16) * 32 + (yi % 2) * 16 + (xi % 16)


def _block_height(rows):
    gobs, bh = (rows + 7) // 8, 1
    while bh < gobs and bh < 16:
        bh *= 2
    return bh


def swizzle(linear, width_bytes, rows):
    addr = _addresses(width_bytes, rows, _block_height(rows))
    out = np.zeros(int(addr.max()) + 1, np.uint8)
    out[addr.ravel()] = np.frombuffer(linear, np.uint8)
    return out.tobytes()


def deswizzle(tiled, width_bytes, rows):
    addr = _addresses(width_bytes, rows, _block_height(rows))
    return np.frombuffer(tiled, np.uint8)[addr].tobytes()


# ---- pictures ------------------------------------------------------------------------------------------------------

def encode_picture(px):
    """A (512, 512, 4) uint8 RGBA picture as the bank stores it: BC7 blocks, block-linear."""
    assert px.shape == (SIZE, SIZE, 4), px.shape
    n = SIZE // 4
    blocks = px.reshape(n, 4, n, 4, 4).transpose(0, 2, 1, 3, 4).reshape(n * n, 16, 4)   # row-major 4x4 blocks
    seen = blocks[:, :, 3].any(1)                    # the empty background is most of a picture: encode one such block
    out = np.empty((n * n, 16), np.uint8)
    out[~seen] = np.frombuffer(bc7.encode(np.zeros((1, 16, 4), np.uint8))[0], np.uint8)
    if seen.any():
        out[seen] = np.frombuffer(b''.join(bc7.encode(blocks[seen])), np.uint8).reshape(-1, 16)
    return swizzle(out.tobytes(), n * 16, n)


# ---- the bank ------------------------------------------------------------------------------------------------------

class Table:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.head = bytearray(f.read(0x30))
            assert self.head[:7] == b'TX12NSW', path
            self.n = struct.unpack_from('<I', self.head, 0x14)[0]
            self.table = bytearray(f.read(self.n * ENTRY))
        self.stored, self.unpacked, self.gap = struct.unpack_from('<III', self.head, 0x0c)[0], \
            struct.unpack_from('<I', self.head, 0x10)[0], struct.unpack_from('<I', self.head, 0x18)[0]
        self.start = 0x30 + self.n * ENTRY + self.gap
        with open(path, 'rb') as f:
            f.seek(0x30 + self.n * ENTRY)
            self.layouts = f.read(self.gap)
        self.layout_count, self.layout_size = struct.unpack_from('<II', self.layouts, 0)
        assert self.layout_size == self.gap, 'layout block size and header gap disagree'

    def entry(self, i):
        return self.table[i * ENTRY:(i + 1) * ENTRY]

    def names(self):
        return [bytes(self.entry(i)[:64]).split(b'\0')[0].decode('latin1') for i in range(self.n)]


def _align(x, a):
    return (x + a - 1) // a * a


def install(game, pictures, log=print):
    """pictures: {texture name: raw bytes from encode_picture}. Rebuilds LIVE.TEX from LIVE.TEX.orig with them added."""
    bank = bank_path(game)
    orig = bank.with_name(bank.name + '.orig')
    if not orig.exists():
        log(f'pictures: keeping the original image bank as {orig.name} (a 1.4 GB copy)')
        shutil.copy2(bank, orig)
    t = Table(orig)
    names = {nm.lower() for nm in t.names()}
    clash = [nm for nm in pictures if nm.lower() in names]
    assert not clash, f'already in the bank: {clash[:3]}'
    tmpl = next(t.entry(i) for i, nm in enumerate(t.names()) if nm.lower() == TEMPLATE)

    layout_hash = bytes(tmpl[0x48:0x50])
    used = 0x10 + 0x40 * t.layout_count
    assert layout_hash in t.layouts[0x10:used], 'template layout is not in the layout block'
    n = t.n + len(pictures)
    start = _align(0x30 + n * ENTRY + used, 0x1000)
    layouts = bytearray(t.layouts[:used])
    struct.pack_into('<I', layouts, 4, start - (0x30 + n * ENTRY))
    shift = start - t.start
    table = bytearray(t.table)
    for i in range(t.n):
        o = i * ENTRY + 0x124
        struct.pack_into('<I', table, o, struct.unpack_from('<I', table, o)[0] + shift)
    off = start + t.stored
    stored = unpacked = 0
    for name, raw in pictures.items():
        assert len(raw) == SIZE * SIZE, (name, len(raw))       # BC7: one byte per texel
        e = bytearray(tmpl)
        e[:64] = name.encode('ascii').ljust(64, b'\0')
        e[0x40:0x48] = hashlib.md5(raw).digest()[:8]
        struct.pack_into('<I', e, 0x50, len(raw))
        struct.pack_into('<II', e, 0xc0, len(raw), len(raw))   # stored as is, no LZ4
        e[0xd0:0xd4] = RAW_FLAGS                               # +0xd0 0: not LZ4 (the template's is 1)
        struct.pack_into('<I', e, 0x124, off)
        table += e
        off += _align(len(raw), 16)
        stored += _align(len(raw), 16)
        unpacked += len(raw)
    head = bytearray(t.head)
    struct.pack_into('<I', head, 0x0c, t.stored + stored)
    struct.pack_into('<I', head, 0x10, t.unpacked + unpacked)
    struct.pack_into('<I', head, 0x14, n)
    struct.pack_into('<I', head, 0x18, start - (0x30 + n * ENTRY))
    assert off < 1 << 32, 'bank would pass 4 GiB'

    tmp = bank.with_name(bank.name + '.tmp')
    with open(orig, 'rb') as src, open(tmp, 'wb') as dst:
        dst.write(head)
        dst.write(table)
        dst.write(layouts)
        dst.write(b'\0' * (start - dst.tell()))
        src.seek(t.start)
        left = t.stored
        while left:
            chunk = src.read(min(left, 64 << 20))
            assert chunk, 'original bank is shorter than its header says'
            dst.write(chunk)
            left -= len(chunk)
        for raw in pictures.values():
            dst.write(raw)
            dst.write(b'\0' * (_align(len(raw), 16) - len(raw)))
        dst.write(b'\0' * (32 * n))                            # the per-entry trailer
    swap_in(tmp, bank)
    log(f'pictures: {len(pictures)} added to {bank.name} ({t.n} -> {n} entries)')


def uninstall(game, log=print):
    bank = bank_path(game)
    orig = bank.with_name(bank.name + '.orig')
    if orig.exists():
        swap_in(orig, bank)
        log(f'equipment: {bank.name} put back')


def verify(game, pictures, log=print):
    """Reads the new bank back: header totals, every original entry's data unchanged in place (sampled), and each
    added picture ({texture name: raw bytes}) byte for byte. Returns the number of problems."""
    bank = bank_path(game)
    orig = bank.with_name(bank.name + '.orig')
    t, o = Table(bank), Table(orig)
    bad = 0
    idx = {nm.lower(): i for i, nm in enumerate(t.names())}
    used = 0x10 + 0x40 * o.layout_count
    if t.layout_count != o.layout_count or t.layouts[8:used] != o.layouts[8:used] or t.layout_size != t.gap:
        log('image bank: the layout block differs from the original')
        bad += 1
    hashes = {bytes(t.layouts[0x10 + 0x40 * k:0x18 + 0x40 * k]) for k in range(t.layout_count)}
    missing = [nm for i, nm in enumerate(t.names()) if bytes(t.entry(i)[0x48:0x50]) not in hashes]
    if missing:
        log(f'image bank: {len(missing)} entries name a layout that is not in the block, first {missing[0]}')
        bad += 1
    tail = t.start + t.stored + 32 * t.n
    if bank.stat().st_size != tail:
        log(f'image bank: size {bank.stat().st_size}, expected {tail}')
        bad += 1
    with open(bank, 'rb') as fb, open(orig, 'rb') as fo:
        names = o.names()
        for i in range(0, o.n, max(1, o.n // 200)):
            eo, en = o.entry(i), t.entry(idx[names[i].lower()])
            if eo[:0x124] != en[:0x124] or eo[0x128:] != en[0x128:]:
                log(f'image bank: {names[i]}: entry changed')
                bad += 1
            c = struct.unpack_from('<I', eo, 0xc4)[0]
            fo.seek(struct.unpack_from('<I', eo, 0x124)[0])
            fb.seek(struct.unpack_from('<I', en, 0x124)[0])
            if fo.read(c) != fb.read(c):
                log(f'image bank: {names[i]}: data differs')
                bad += 1
        for name, raw in pictures.items():
            e = t.entry(idx[name.lower()])
            fb.seek(struct.unpack_from('<I', e, 0x124)[0])
            if fb.read(struct.unpack_from('<I', e, 0xc4)[0]) != raw:
                log(f'image bank: {name} is not the picture that was added')
                bad += 1
    if bad:
        log(f'image bank: {bad} problems')
    return bad
