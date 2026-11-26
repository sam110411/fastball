"""Road to the Show equipment, offline.

Offline, the Equipment screen in Road to the Show says "You do not own any of this item": equipment comes from the
online Packages, and the local item catalog the game falls back on (FRONTEND/RTTS/INVENTORYDATA.XML) lists only pitch
types, archetypes and perks. This tool adds every enabled piece of equipment from the game's own equipment list
(FRONTEND/EQUIPMENT/MLBEQUIPMENT.JSON), the pregame rituals and the socks to that catalog, and lists them as owned in
the starting inventory (FRONTEND/RTTS/INVQUANTITYDATA.XML).

    python -I tools/equipment/equipment.py install   --game <install folder> [--no-own] [--no-pictures]
    python -I tools/equipment/equipment.py uninstall --game <install folder>
    python -I tools/equipment/equipment.py status    --game <install folder>

What an item does is the game's own rule: its slot's boost to a perk category at its rarity
(FRONTEND/RTTS/EQUIPMENTDESCRIPTIONS.XML: a Gold bat gives +4% to hitting perks). Each item's rarity is in
rarities.json; an item that is not listed there is Silver.

Every item gets a picture, made on this PC from the item's own model and textures in the game files (pictures.py; the
first install takes a few minutes for that, later ones reuse them) and added to the game's image bank
(live_bank.py). An item that cannot be rendered has an empty picture; --no-pictures skips the pictures altogether.

--no-own adds the items to the catalog without owning any of them (for a setup where something else hands them out).

Four files of game/romfs change: the two XML files above, MLBEQUIPMENT.JSON (see mobile_enable) and
NSW/LIVECONTENT/LIVE.TEX, plus their sizes in game/romfs_list.tsv. The original of each is kept beside it as
<name>.orig; install always starts from the originals, so it can be run again, and uninstall puts them back. A changed
file is written beside the old one and swapped in, never written in place. Close the game first.
"""
import argparse
import json
import re
import shutil
import sys
from pathlib import Path
from xml.sax.saxutils import quoteattr

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:                 # python -I leaves the script's folder out
    sys.path.insert(0, str(HERE))
import live_bank  # noqa: E402
import pictures  # noqa: E402
import romfs_index  # noqa: E402
from swap import swap_in  # noqa: E402

ID_BASE = 800000          # our item ids: ID_BASE + equipment id; the game's own items are in the 46000-50000 range
MARK = 'rtts-equipment'   # the comment around what this tool adds to the two XML files
RARITIES = HERE / 'rarities.json'   # [{key, slot, brand, name, rarity}]; key: e<equipment id>, RITUAL_<picture>, SOCKS
RARITY_NAMES = ['Common', 'Bronze', 'Silver', 'Gold', 'Diamond']
DEFAULT_RARITY = 2        # Silver, for an item rarities.json does not list
SOCKS_PICTURE = 'SOCK_STANCE_THESHOW26'

# MLBEQUIPMENT.JSON "type" -> inventory slot_type_id (INVENTORYDATA.XML <slot_types>, EQUIPMENTDESCRIPTIONS.XML)
EQ_TYPE_TO_SLOT = {0: 0, 1: 1, 3: 2, 2: 3, 10: 4, 11: 5, 15: 6, 9: 7, 12: 8, 14: 9, 13: 10, 8: 11, 7: 12, 17: 15}
SLOT_NAMES = {0: 'Bat', 1: 'Batting Gloves', 2: 'Fielding Glove', 3: 'Cleats', 4: 'Catcher Mask',
              5: 'Chest Protector', 6: 'Leg Guards', 7: 'Elbow Guard', 8: 'Shin Guard', 9: 'Wrist Guard',
              10: 'Compression Sleeve', 11: 'Bat Grip', 12: 'Eyewear', 13: 'Ritual', 14: 'Socks', 15: 'Sliding Mitt'}


def paths(game):
    """The three text files this tool changes (the fourth file is live_bank.bank_path)."""
    rt = game / 'game' / 'romfs' / 'FRONTEND'
    return rt / 'RTTS' / 'INVENTORYDATA.XML', rt / 'RTTS' / 'INVQUANTITYDATA.XML', rt / 'EQUIPMENT' / 'MLBEQUIPMENT.JSON'


def original(f):
    """A file's content as the game shipped it: its .orig when this tool has changed it."""
    orig = f.with_name(f.name + '.orig')
    return orig if orig.exists() else f


def write_text(path, text, newline):
    """Replace path with text: written as a new file, then swapped in."""
    tmp = path.with_name(path.name + '.new')
    tmp.write_text(text, encoding='utf-8', newline=newline)
    swap_in(tmp, path)


def rarity_list():
    """rarities.json as {item key: its row}."""
    return {r['key']: r for r in json.loads(RARITIES.read_text(encoding='utf-8'))}


def brand_ids(inv_text):
    """Inventory brand name (lowercase, without the ^r^ mark) -> brand_id."""
    out = {}
    for m in re.finditer(r'<brand id="(\d+)" name="([^"]*)"', inv_text):
        out[m.group(2).replace('^r^', '').strip().lower()] = int(m.group(1))
    return out


def equipment_items(eq, brands, live, table):
    """The items to add: one per enabled entry of the equipment list `eq`, one per ritual picture in `live`
    (CROSS/LIVE) and the socks. table: rarity_list()."""
    def rarity(key):
        return RARITY_NAMES.index(table[key]['rarity']) if key in table else DEFAULT_RARITY

    makers = {m['id']: m['name'] for m in eq['makers']}
    items = []
    for e in eq['eqlist']:
        slot = EQ_TYPE_TO_SLOT.get(e['type'])
        name = e['name'].strip()
        # not items: disabled entries, tier -1 and the "OFF" entries (a slot's "nothing equipped" choice)
        if slot is None or not e.get('enb') or e.get('tier', 0) < 0 or name.upper() == 'OFF':
            continue
        maker = makers.get(e['brand'], '')
        if not maker or maker.startswith('z'):       # zGeneric: unbranded
            full = name
        elif name.startswith(':'):                   # the legends' bats: "TheShow" + ": HWagner W107"
            full = f'{maker}: {name[1:].strip()}'
        else:
            full = f'{maker} {name}'
        # servoy_equipment_id is written as it is into the player's appearance when the item is equipped
        # (main+0xbcf47c -> main+0x6a8190 in 1.0.20), and every equipment loader looks that value up as the entry's
        # "style" (record +6, main+0x17578b0), not its "id". For a bat, a value above 129 is a legend / skin index
        # (value - 130), so the id would give some bats another bat's skin.
        items.append(dict(id=ID_BASE + e['id'], slot=slot, brand=brands.get(maker.lower(), -1), servoy=e['style'],
                          eqid=e['id'], name=full, sock=-1, rarity=rarity(f'e{e["id"]}'), desc=SLOT_NAMES[slot]))
    # Rituals and socks are not in the equipment list. The game ships their pictures (CROSS/LIVE/RITUAL_*.WEBP,
    # SOCK_STANCE_*.WEBP, and the same names in its image bank): one ritual item per picture, and one socks item.
    for k, stem in enumerate(sorted(p.stem for p in live.glob('RITUAL_*.WEBP'))):
        name = table[stem]['name'] if stem in table else stem[7:].replace('_', ' ').title()
        items.append(dict(id=ID_BASE + 900000 + 100 + k, slot=13, brand=0, servoy=-1, sock=-1, texture=stem, name=name,
                          rarity=rarity(stem), desc=SLOT_NAMES[13]))
    if (live / f'{SOCKS_PICTURE}.WEBP').exists():
        items.append(dict(id=ID_BASE + 900000 + 14, slot=14, brand=-1, servoy=-1, sock=0, texture=SOCKS_PICTURE,
                          name=table['SOCKS']['name'] if 'SOCKS' in table else 'Socks', rarity=rarity('SOCKS'),
                          desc=SLOT_NAMES[14]))
    return items


def item_xml(it):
    a = [('brand_id', it['brand']), ('description', it['desc']), ('id', it['id']), ('is_new', 0), ('is_official', 1),
         ('name', it['name']), ('rarity_id', it['rarity']), ('servoy_equipment_id', it['servoy']),
         ('slot_type_id', it['slot']), ('sock_type_id', it['sock']), ('style_name', ''),
         ('texture_name', it.get('texture') or ''), ('type_id', 2)]
    return '\t\t<item ' + ' '.join(f'{k}={quoteattr(str(v))}' for k, v in a) + '/>'


def insert_before_items_end(text, lines):
    """text with `lines` added as the last children of its <items>, between two marker comments."""
    i = text.rindex('\t</items>')
    block = f'\t\t<!-- {MARK} start -->\r\n' + ''.join(line + '\r\n' for line in lines) + f'\t\t<!-- {MARK} end -->\r\n'
    return text[:i] + block + text[i:]


def mobile_enable(eqf, eq, items):
    """Write MLBEQUIPMENT.JSON with "enb_mobile": 1 on every added item.

    This Switch build runs the game's "mobile" mode (flag main+0x463cb48 in 1.0.20): an equipped item whose enb_mobile
    is not 1 is swapped for the first enb_mobile item of the same type and maker (bats, main+0x16cc5f0 /
    main+0x1757b80; the legends' bats fall back to a plain bat), a bat grip without it is not applied at all
    (main+0x16cb8c0), and the shoe and eyewear models of items without it are not preloaded (main+0x1750ac0,
    main+0x1759e10). Only 158 of the 718 entries have the flag, so most added items would not show on the player.
    The file is written as the game's tool wrote it (indent 4, CRLF). -> how many entries changed."""
    ids = {it['eqid'] for it in items if 'eqid' in it}
    n = 0
    for e in eq['eqlist']:
        if e['id'] in ids and e.get('enb_mobile') != 1:
            e['enb_mobile'] = 1
            n += 1
    write_text(eqf, json.dumps(eq, indent=4, ensure_ascii=False), '\r\n')
    return n


def add_pictures(game, items, log):
    """Make each equipment item's picture from the game files (pictures.py), add the pictures to the game's image
    bank (the game reads item pictures from NSW/LIVECONTENT/LIVE.TEX only) and point the items at them. An item that
    could not be rendered keeps an empty picture."""
    made = pictures.generate(game, log=log)[0]
    raw = {}
    for it in items:
        name = f'RTTSMOD_EQ_{it.get("eqid")}'        # rituals and socks (no eqid) use the game's own pictures
        if name in made:
            raw[name] = made[name].read_bytes()
            it['texture'] = name
    if not raw:
        live_bank.uninstall(game, log)
        return
    live_bank.install(game, raw, log)
    if live_bank.verify(game, raw, log):
        sys.exit('The image bank did not come out as written. Run "uninstall" to put the game files back.')


def install(game, own=True, with_pictures=True, log=print):
    inv, qty, eqf = paths(game)
    for f in (inv, qty, eqf):
        orig = f.with_name(f.name + '.orig')
        if not orig.exists():
            shutil.copy2(f, orig)
    inv_text = original(inv).read_text(encoding='utf-8')
    qty_text = original(qty).read_text(encoding='utf-8')
    eq = json.loads(original(eqf).read_text(encoding='utf-8'))
    items = equipment_items(eq, brand_ids(inv_text), game / 'game' / 'romfs' / 'CROSS' / 'LIVE', rarity_list())
    mobile_enable(eqf, eq, items)
    if with_pictures:
        add_pictures(game, items, log)
    else:
        live_bank.uninstall(game, log)
    write_text(inv, insert_before_items_end(inv_text, [item_xml(it) for it in items]), '')
    owned = [f'\t\t<item id="{it["id"]}" quantity="1"/>' for it in items] if own else []
    write_text(qty, insert_before_items_end(qty_text, owned), '')
    romfs_index.sync(game, romfs_index.FILES)
    by_rarity = {}
    for it in items:
        by_rarity[it['rarity']] = by_rarity.get(it['rarity'], 0) + 1
    log(f'equipment: {len(items)} items added (' +
        ', '.join(f'{n} {RARITY_NAMES[r]}' for r, n in sorted(by_rarity.items())) + '), ' +
        ('all owned' if own else 'none owned'))


def uninstall(game, log=print):
    live_bank.uninstall(game, log)
    for f in paths(game):
        orig = f.with_name(f.name + '.orig')
        if orig.exists():
            swap_in(orig, f)
            log(f'equipment: {f.name} put back')
    romfs_index.sync(game, romfs_index.FILES)


def state(game):
    """How the install stands: {'installed': bool, 'items': n, 'owned': n, 'pictures': n}."""
    inv, qty, _ = paths(game)
    text = inv.read_text(encoding='utf-8') if inv.exists() else ''
    if MARK not in text:
        return dict(installed=False, items=0, owned=0, pictures=0)
    owned = qty.read_text(encoding='utf-8').split(MARK)
    bank = live_bank.bank_path(game)
    names = live_bank.Table(bank).names() if bank.exists() else []
    return dict(installed=True, items=text.split(MARK)[1].count('<item '),
                owned=owned[1].count('<item ') if len(owned) > 2 else 0,
                pictures=sum(1 for n in names if n.startswith('RTTSMOD_EQ_')))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('action', choices=['install', 'uninstall', 'status'])
    ap.add_argument('--game', type=Path, required=True, help='the install folder (the one with mlb.exe)')
    ap.add_argument('--no-own', action='store_true', help='add the items to the catalog without owning them')
    ap.add_argument('--no-pictures', action='store_true', help='add the items without making their pictures')
    a = ap.parse_args()
    if not paths(a.game)[0].exists():
        sys.exit(f'{paths(a.game)[0]} not found: --game takes the install folder')

    def log(text):
        print(text, flush=True)

    if a.action == 'install':
        install(a.game, not a.no_own, not a.no_pictures, log)
    elif a.action == 'uninstall':
        uninstall(a.game, log)
    else:
        s = state(a.game)
        log(f'equipment: {s["items"]} items, {s["owned"]} owned, {s["pictures"]} pictures in the image bank'
            if s['installed'] else 'equipment: not installed')


if __name__ == '__main__':
    main()
