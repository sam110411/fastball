"""Tests for tools/equipment that need no game files: every input is made here.

    python -I -m unittest discover -s tools/equipment/tests -v        (needs numpy and lz4, as the tool does)
"""
import contextlib
import io
import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

TOOL = Path(__file__).resolve().parents[1]
for p in (TOOL, TOOL / 'render'):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))
import bc7  # noqa: E402
import equipment  # noqa: E402
import live_bank  # noqa: E402
import romfs_index  # noqa: E402
import swap  # noqa: E402
import tx12  # noqa: E402

QUIET = lambda text: None   # noqa: E731


def bits_to_block(fields):
    """A 16-byte block from (value, bit count) fields, lowest bit first."""
    n = pos = 0
    for value, count in fields:
        n |= value << pos
        pos += count
    assert pos <= 128
    return np.frombuffer(n.to_bytes(16, 'little'), np.uint8).reshape(1, 16)


class Decoders(unittest.TestCase):
    def test_bc1_four_colours(self):
        # c0 = pure red (0xF800) > c1 = pure blue (0x001F): palette red, blue, 2/3 red, 1/3 red; indices 0, 1, 2, 3
        block = np.frombuffer(struct.pack('<HHI', 0xF800, 0x001F, 0b11100100), np.uint8).reshape(1, 8)
        px = tx12._color_blocks(block, False)[0]
        self.assertEqual(px[0].tolist(), [255, 0, 0, 255])
        self.assertEqual(px[1].tolist(), [0, 0, 255, 255])
        self.assertEqual(px[2].tolist(), [170, 0, 85, 255])
        self.assertEqual(px[3].tolist(), [85, 0, 170, 255])

    def test_bc1_three_colours_and_transparent(self):
        block = np.frombuffer(struct.pack('<HHI', 0x001F, 0xF800, 0b1110), np.uint8).reshape(1, 8)   # c0 <= c1
        px = tx12._color_blocks(block, False)[0]
        self.assertEqual(px[0].tolist(), [127, 0, 127, 255])    # index 2: the middle
        self.assertEqual(px[1].tolist(), [0, 0, 0, 0])          # index 3: transparent black

    def test_bc4_ramp(self):
        # a0 = 255 > a1 = 0: eight values; texel k takes index k % 8
        lut = sum((k % 8) << (3 * k) for k in range(16))
        block = np.frombuffer(bytes([255, 0]) + lut.to_bytes(6, 'little'), np.uint8).reshape(1, 8)
        v = tx12._alpha_blocks(block)[0]
        self.assertEqual(v[:8].tolist(), [255, 0, 218, 182, 145, 109, 72, 36])

    def test_bc7_mode6_endpoints(self):
        # mode 6: 7-bit RGBA endpoints + a p-bit each, 4-bit indices. Endpoint 0 all zero, endpoint 1 all ones.
        fields = [(1 << 6, 7)] + [(0, 7), (127, 7)] * 4 + [(0, 1), (1, 1)] + [(0, 3)] + [(15, 4)] * 15
        px = tx12._bc7_blocks(bits_to_block(fields))[0]
        self.assertEqual(px[0].tolist(), [0, 0, 0, 0])              # index 0: endpoint 0
        self.assertEqual(px[1].tolist(), [255, 255, 255, 255])      # index 15: endpoint 1

    def test_bc7_empty_block(self):
        self.assertEqual(int(tx12._bc7_blocks(np.zeros((1, 16), np.uint8)).sum()), 0)

    def test_bc7_encoder_and_decoder_agree(self):
        rng = np.random.default_rng(1)
        ramp = np.linspace(0, 255, 16)[None, :, None]
        blocks = np.clip(ramp * rng.uniform(0.2, 1, (64, 1, 4)) + rng.uniform(0, 40, (64, 1, 4)), 0, 255)
        blocks = blocks.astype(np.uint8)
        blocks[:8, :, 3] = 0                                         # some fully transparent, some opaque
        blocks[8:16, :, 3] = 255
        data = np.frombuffer(b''.join(bc7.encode(blocks)), np.uint8).reshape(-1, 16)
        back = tx12._bc7_blocks(data)
        self.assertTrue((back[:8, :, 3] == 0).all())
        seen = blocks[8:].astype(int)
        self.assertLess(np.abs(back[8:] - seen).mean(), 4)

    def test_tiling_round_trip(self):
        rng = np.random.default_rng(2)
        for rows in (1, 8, 32, 128):
            linear = rng.integers(0, 256, rows * 16 * rows, dtype=np.uint8).tobytes()
            tiled = live_bank.swizzle(linear, rows * 16, rows)
            self.assertEqual(live_bank.deswizzle(tiled, rows * 16, rows), linear)
            self.assertEqual(tx12.deswizzle(tiled, rows * 16, rows, tx12.default_block_height(rows)), linear)


def make_bank(path, names):
    """A small TX12NSW bank in the layout live_bank.py documents: `names` entries of 16 bytes each, one layout."""
    layout_hash = b'LAYOUT01'
    n = len(names)
    layouts = bytearray(struct.pack('<II', 1, 0) + bytes(8) + layout_hash.ljust(0x40, b'\0'))
    start = live_bank._align(0x30 + n * live_bank.ENTRY + len(layouts), 0x1000)
    gap = start - (0x30 + n * live_bank.ENTRY)
    struct.pack_into('<I', layouts, 4, gap)
    table, data = bytearray(), bytearray()
    for i, name in enumerate(names):
        e = bytearray(live_bank.ENTRY)
        e[:len(name)] = name.encode()
        e[0x48:0x50] = layout_hash
        payload = bytes([i + 1]) * 16
        struct.pack_into('<II', e, 0xc0, 16, 16)
        struct.pack_into('<I', e, 0x124, start + len(data))
        e[0x12c] = 22
        table += e
        data += payload
    head = bytearray(0x30)
    head[:8] = b'TX12NSW\0'
    struct.pack_into('<IIIII', head, 0x08, 0xc, len(data), len(data), n, gap)
    path.write_bytes(bytes(head) + bytes(table) + bytes(layouts).ljust(gap, b'\0') + bytes(data) + bytes(32 * n))


def make_game(root):
    """An install folder with just what the tool reads and writes."""
    romfs = root / 'game' / 'romfs'
    for sub in ('FRONTEND/RTTS', 'FRONTEND/EQUIPMENT', 'NSW/LIVECONTENT', 'CROSS/LIVE'):
        (romfs / sub).mkdir(parents=True)
    catalog = ('<response>\r\n\t<brands>\r\n\t\t<brand id="7" name="Acme^r^"/>\r\n\t</brands>\r\n\t<items>\r\n'
               '\t\t<item id="46885" name="Four-Seam Fastball" type_id="11"/>\r\n\t</items>\r\n</response>\r\n')
    (romfs / 'FRONTEND/RTTS/INVENTORYDATA.XML').write_text(catalog, encoding='utf-8', newline='')
    (romfs / 'FRONTEND/RTTS/INVQUANTITYDATA.XML').write_text(
        '<response>\r\n\t<items>\r\n\t\t<item id="46885" quantity="1"/>\r\n\t</items>\r\n</response>\r\n',
        encoding='utf-8', newline='')
    eq = {'makers': [{'id': 1, 'name': 'Acme'}, {'id': 2, 'name': 'zGeneric'}, {'id': 3, 'name': 'TheShow'}],
          'eqlist': [
              {'id': 5, 'enb': 1, 'enb_mobile': 0, 'type': 0, 'name': 'Slugger ', 'brand': 1, 'tier': 1, 'style': 9},
              {'id': 6, 'enb': 1, 'enb_mobile': 1, 'type': 2, 'name': 'Gen1', 'brand': 2, 'tier': 0, 'style': 1},
              {'id': 7, 'enb': 1, 'enb_mobile': 0, 'type': 0, 'name': ': HWagner W107', 'brand': 3, 'tier': 33,
               'style': 163},
              {'id': 8, 'enb': 0, 'enb_mobile': 0, 'type': 0, 'name': 'Disabled', 'brand': 1, 'tier': 1, 'style': 3},
              {'id': 9, 'enb': 1, 'enb_mobile': 0, 'type': 17, 'name': 'OFF', 'brand': 2, 'tier': 0, 'style': 0},
              {'id': 10, 'enb': 1, 'enb_mobile': 0, 'type': 0, 'name': 'Hidden', 'brand': 1, 'tier': -1, 'style': 4}]}
    (romfs / 'FRONTEND/EQUIPMENT/MLBEQUIPMENT.JSON').write_text(json.dumps(eq, indent=4), encoding='utf-8',
                                                                newline='\r\n')
    for name in ('RITUAL_GUM', 'SOCK_STANCE_THESHOW26'):
        (romfs / 'CROSS/LIVE' / f'{name}.WEBP').write_bytes(b'')
    make_bank(romfs / 'NSW/LIVECONTENT/LIVE.TEX', ['Ritual_Gum', 'Other_Picture'])
    index = ''.join(f'{(romfs / rel).stat().st_size}\t0\t/{rel}\r\n' for rel in romfs_index.FILES)
    (root / 'game' / 'romfs_list.tsv').write_text(index + '5\t0\t/SOMETHING/ELSE.BIN\r\n', encoding='utf-8', newline='')
    return root


def snapshot(root):
    return {str(p.relative_to(root)): p.read_bytes() for p in sorted(root.rglob('*')) if p.is_file()}


class Bank(unittest.TestCase):
    def test_add_check_and_put_back(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = make_game(Path(tmp))
            before = snapshot(game)
            picture = live_bank.encode_picture(np.full((512, 512, 4), 200, np.uint8))
            self.assertEqual(len(picture), 512 * 512)
            live_bank.install(game, {'RTTSMOD_EQ_5': picture}, QUIET)
            self.assertEqual(live_bank.verify(game, {'RTTSMOD_EQ_5': picture}, QUIET), 0)
            names = live_bank.Table(live_bank.bank_path(game)).names()
            self.assertEqual(names, ['Ritual_Gum', 'Other_Picture', 'RTTSMOD_EQ_5'])
            self.assertEqual(live_bank.verify(game, {'RTTSMOD_EQ_5': bytes(512 * 512)}, QUIET), 1)   # not that picture
            live_bank.uninstall(game, QUIET)
            self.assertEqual(snapshot(game), before)

    def test_install_twice_starts_from_the_original(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = make_game(Path(tmp))
            picture = live_bank.encode_picture(np.zeros((512, 512, 4), np.uint8))
            live_bank.install(game, {'RTTSMOD_EQ_5': picture}, QUIET)
            live_bank.install(game, {'RTTSMOD_EQ_6': picture}, QUIET)
            self.assertEqual(live_bank.Table(live_bank.bank_path(game)).names(),
                             ['Ritual_Gum', 'Other_Picture', 'RTTSMOD_EQ_6'])


class Install(unittest.TestCase):
    def install(self, game, **kw):
        equipment.install(game, with_pictures=False, log=QUIET, **kw)

    def test_items(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = make_game(Path(tmp))
            self.install(game)
            catalog = equipment.paths(game)[0].read_text(encoding='utf-8')
            added = catalog.split(equipment.MARK)[1]
            self.assertEqual(added.count('<item '), 5)       # two bats, the cleats, the ritual and the socks
            self.assertIn('name="Acme Slugger"', added)      # maker + name
            self.assertIn('name="Gen1"', added)              # unbranded
            self.assertIn('name="TheShow: HWagner W107"', added)
            for gone in ('Disabled', 'OFF', 'Hidden'):
                self.assertNotIn(f'name="{gone}"', added)
            self.assertIn('id="800005"', added)
            self.assertIn('servoy_equipment_id="9"', added)  # the entry's style, not its id
            self.assertIn('brand_id="7"', added)             # "Acme^r^" in the game's brand list
            self.assertIn('Four-Seam Fastball', catalog)     # what was there stays
            self.assertEqual(equipment.state(game), dict(installed=True, items=5, owned=5, pictures=0))

    def test_every_added_entry_is_marked_for_this_build(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = make_game(Path(tmp))
            self.install(game)
            eq = json.loads(equipment.paths(game)[2].read_text(encoding='utf-8'))
            flags = {e['id']: e['enb_mobile'] for e in eq['eqlist']}
            self.assertEqual(flags, {5: 1, 6: 1, 7: 1, 8: 0, 9: 0, 10: 0})

    def test_rarity_comes_from_the_list(self):
        rows = json.loads(equipment.RARITIES.read_text(encoding='utf-8'))
        self.assertEqual(len({r['key'] for r in rows}), len(rows))
        self.assertTrue(all(r['rarity'] in equipment.RARITY_NAMES for r in rows))
        table = {'e5': dict(rarity='Diamond'), 'RITUAL_GUM': dict(rarity='Gold', name='Bubble Gum')}
        with tempfile.TemporaryDirectory() as tmp:
            game = make_game(Path(tmp))
            eq = json.loads(equipment.paths(game)[2].read_text(encoding='utf-8'))
            items = {it['name']: it for it in equipment.equipment_items(eq, {}, game / 'game/romfs/CROSS/LIVE', table)}
            self.assertEqual(items['Acme Slugger']['rarity'], 4)
            self.assertEqual(items['Bubble Gum']['rarity'], 3)
            self.assertEqual(items['Gen1']['rarity'], equipment.DEFAULT_RARITY)

    def test_not_owned(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = make_game(Path(tmp))
            self.install(game, own=False)
            self.assertEqual(equipment.state(game), dict(installed=True, items=5, owned=0, pictures=0))

    def test_uninstall_puts_every_file_back(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = make_game(Path(tmp))
            before = snapshot(game)
            self.install(game)
            self.install(game, own=False)                    # again, with other options: still from the originals
            self.assertNotEqual(snapshot(game), before)
            equipment.uninstall(game, QUIET)
            self.assertEqual(snapshot(game), before)
            self.assertFalse(equipment.state(game)['installed'])

    def test_index_follows_the_sizes(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = make_game(Path(tmp))
            self.install(game)
            listed = {}
            for line in (game / 'game' / 'romfs_list.tsv').read_text(encoding='utf-8').splitlines():
                size, _, path = line.split('\t')
                listed[path[1:]] = int(size)
            for rel in romfs_index.FILES:
                self.assertEqual(listed[rel], (game / 'game' / 'romfs' / rel).stat().st_size)
            self.assertEqual(listed['SOMETHING/ELSE.BIN'], 5)

    def test_works_where_windows_refuses_renames_onto_xml(self):
        # issue #24: a player's PC answers every rename onto an .XML with WinError 17 (ERROR_NOT_SAME_DEVICE)
        real = os.replace

        def replace(src, dst):
            if str(dst).upper().endswith('.XML'):
                raise OSError(18, 'The system cannot move the file to a different disk drive', str(src), 17, str(dst))
            real(src, dst)

        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(swap.os, 'replace', replace), \
                mock.patch.object(swap.time, 'sleep'), contextlib.redirect_stdout(io.StringIO()):
            game = make_game(Path(tmp))
            before = snapshot(game)
            self.install(game)
            self.assertEqual(equipment.state(game), dict(installed=True, items=5, owned=5, pictures=0))
            self.assertEqual(list(game.rglob('*.new')), [])
            equipment.uninstall(game, QUIET)
            self.assertEqual(snapshot(game), before)


if __name__ == '__main__':
    unittest.main()
