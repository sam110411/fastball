"""Keeps the install's file index (game/romfs_list.tsv: size, offset, /PATH per line) true to the game files the
equipment tool changes. The runtime's RomFS check (mlb.exe --probe-fs) sizes a sample of the listed files against it.
"""
from swap import swap_in

FILES =['FRONTEND/RTTS/INVENTORYDATA.XML', 'FRONTEND/RTTS/INVQUANTITYDATA.XML',
         'FRONTEND/EQUIPMENT/MLBEQUIPMENT.JSON', 'NSW/LIVECONTENT/LIVE.TEX']


def sync(game, rels):
    """Set the listed size of each romfs-relative path in rels to the file's size now. -> lines changed."""
    tsv = game / 'game' / 'romfs_list.tsv'
    if not tsv.exists():
        return 0
    romfs = game / 'game' / 'romfs'
    want = {'/' + r.upper(): (romfs / r).stat().st_size for r in rels if (romfs / r).exists()}
    lines = tsv.read_text(encoding='utf-8', newline='').split('\n')
    changed = 0
    for i, line in enumerate(lines):
        p = line.split('\t')
        if len(p) == 3 and p[2].rstrip('\r').upper() in want:
            size = str(want[p[2].rstrip('\r').upper()])
            if p[0] != size:
                p[0] = size
                lines[i] = '\t'.join(p)
                changed += 1
    if changed:
        tmp = tsv.with_name(tsv.name + '.new')
        tmp.write_text('\n'.join(lines), encoding='utf-8', newline='')
        swap_in(tmp, tsv)
    return changed
