"""Makes the equipment item pictures on the player's PC, from their own game files.

The game has no picture for an equipment item offline (they come with the online catalog), so each one is rendered
from the item's own model and textures in the install (render/) and kept, ready for the game's image bank
(live_bank.py), in <install folder>/equipment-pictures/RTTSMOD_EQ_<equipment id>.bc7. Nothing is downloaded. The
first run renders every item (a few minutes, on several processor cores at low priority); later runs only make what
is missing. The folder is emptied when the renderer or the game's equipment list changes.

    python -I tools/equipment/pictures.py --game <install folder> [--workers N] [--ids 54,1,2]

An item whose model or textures the install lacks has no picture; it is named in the log and not tried again.

Needs numpy and lz4 (the setup's Python has both).
"""
import argparse
import hashlib
import json
import multiprocessing
import os
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
for _p in (HERE, HERE / 'render'):
    if str(_p) not in sys.path:
        sys.path.insert(0, str(_p))
import live_bank  # noqa: E402
import render_all  # noqa: E402
from swap import swap_in  # noqa: E402

RENDER_VERSION = '1'      # raise when the renderer's pictures change: everything is made again
NONE = 'no_picture.json'  # in the cache: {equipment id: why} for the items that cannot be rendered (not tried again)


def romfs_of(game):
    return Path(game) / 'game' / 'romfs'


def cache_dir(game):
    """Where an install keeps its pictures: beside the game, so a newer setup finds them again."""
    return Path(game) / 'equipment-pictures'


def _stamp(romfs):
    h = hashlib.sha1(RENDER_VERSION.encode())
    h.update(render_all.equipment_list(romfs).read_bytes())
    return h.hexdigest()[:16]


def _prepare(cache, romfs):
    """The cache folder, emptied when it was made by another renderer version or for another equipment list."""
    cache.mkdir(parents=True, exist_ok=True)
    mark, stamp = cache / 'stamp.txt', _stamp(romfs)
    if not mark.exists() or mark.read_text().strip() != stamp:
        for f in list(cache.glob('*.bc7')) + list(cache.glob('*.tmp')) + list(cache.glob(NONE)):
            f.unlink()
        mark.write_text(stamp + '\n')


def _write(path, raw):
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_bytes(raw)
    swap_in(tmp, path)


# ---- one process's share of the work ----------------------------------------------------------------------------------

_ctx = _mods = _items = None


def _start(romfs, background):
    global _ctx, _mods, _items
    if background and os.name == 'nt':               # leave the PC usable (a game may be running): below normal
        try:
            import ctypes
            k = ctypes.windll.kernel32
            k.GetCurrentProcess.restype = ctypes.c_void_p
            k.SetPriorityClass.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            k.SetPriorityClass(k.GetCurrentProcess(), 0x4000)
        except Exception:
            pass
    _ctx = render_all.Ctx(romfs)
    _mods = render_all.modules()
    _items = {e['id']: e for e in _ctx.items}


def _make(job):
    """Render and store one item's picture. -> (equipment id, '' or why it has none, whether that will stay so).
    Only an item without a renderer or without its model or textures stays without a picture; anything else that
    goes wrong (the PC short of memory, a file busy) is worth another try."""
    eid, cache = job
    try:
        e = _items[eid]
        if not any(e['type'] in m.TYPES for m in _mods):
            return eid, f'no renderer for type {e["type"]}', True
        px = next(m for m in _mods if e['type'] in m.TYPES).render_item(e, _ctx)
        if px is None:
            return eid, 'its model or textures were not found', True
        _write(Path(cache) / f'RTTSMOD_EQ_{eid}.bc7', live_bank.encode_picture(px))
        return eid, '', False
    except Exception as ex:
        return eid, f'{type(ex).__name__}: {ex}', False


# ---- all of them ------------------------------------------------------------------------------------------------------

def generate(game, cache=None, ids=None, workers=None, log=print):
    """Make the picture of every equipment item that has none in the cache yet (cache: cache_dir(game) by default).
    -> ({texture name: its file in the cache}, {equipment id: why it has no picture})."""
    romfs, cache = romfs_of(game), Path(cache or cache_dir(game))
    _prepare(cache, romfs)
    ctx = render_all.Ctx(romfs)
    items = [e for e in ctx.items if ids is None or e['id'] in ids]
    items.sort(key=lambda e: (e['type'], e['brand'], e['id']))   # neighbours share models and textures
    known = json.loads((cache / NONE).read_text()) if (cache / NONE).exists() else {}
    failed = {e['id']: known[str(e['id'])] for e in items if str(e['id']) in known}
    todo = [e['id'] for e in items if e['id'] not in failed and not (cache / f'RTTSMOD_EQ_{e["id"]}.bc7').exists()]
    if todo:
        if workers is None:
            workers = max(1, min(8, (os.cpu_count() or 2) // 2))
        workers = max(1, min(workers, (len(todo) + 7) // 8))
        log(f'pictures: making {len(todo)} equipment picture{"s" if len(todo) > 1 else ""} from your game files on '
            f'{workers} processor core{"s" if workers > 1 else ""} (only the first time)')
        t0 = last = time.time()
        jobs = [(eid, str(cache)) for eid in todo]

        lasting = set()

        def note(n, eid, why, stays):
            nonlocal last
            if why:
                failed[eid] = why
                if stays:
                    lasting.add(eid)
            now = time.time()
            if n == len(todo) or now - last >= 10:
                last = now
                left = (now - t0) / n * (len(todo) - n)
                log(f'pictures: {n} of {len(todo)}' + (f', about {left / 60:.0f} min left' if left >= 90 else
                                                       f', about {left:.0f} s left' if n < len(todo) else
                                                       f' in {now - t0:.0f} s'))

        if workers == 1:
            _start(str(romfs), False)
            for n, job in enumerate(jobs, 1):
                note(n, *_make(job))
        else:
            with multiprocessing.Pool(workers, _start, (str(romfs), True)) as pool:
                for n, res in enumerate(pool.imap_unordered(_make, jobs, chunksize=4), 1):
                    note(n, *res)
        again = [eid for eid in failed if eid in todo and eid not in lasting]
        if again:                                    # something passing went wrong: once more, one at a time
            log(f'pictures: trying {len(again)} again')
            if _ctx is None:
                _start(str(romfs), False)
            for eid in again:
                _, why, stays = _make((eid, str(cache)))
                if why:
                    failed[eid] = why
                    if stays:
                        lasting.add(eid)
                else:
                    del failed[eid]
        known.update({str(eid): failed[eid] for eid in lasting})
        (cache / NONE).write_text(json.dumps(known, indent=1))
    made = {}
    for e in items:
        f = cache / f'RTTSMOD_EQ_{e["id"]}.bc7'
        if f.exists():
            made[f.stem] = f
        elif e['id'] not in failed:
            failed[e['id']] = 'no picture'
    names = {e['id']: f'{ctx.maker(e)} {e["name"].strip()}'.strip() for e in items}
    for eid, why in sorted(failed.items()):
        log(f'pictures: no picture for {names[eid]} (id {eid}): {why}')
    return made, failed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', type=Path, required=True, help='the install folder (the one with mlb.exe)')
    ap.add_argument('--cache', type=Path, help='where the pictures are kept (default: equipment-pictures in --game)')
    ap.add_argument('--workers', type=int, help='processor cores to use (default: half of them, at most 8)')
    ap.add_argument('--ids', help='comma-separated equipment ids (default: every item)')
    a = ap.parse_args()
    ids = {int(x) for x in a.ids.split(',')} if a.ids else None
    made, failed = generate(a.game, a.cache, ids, a.workers, lambda s: print(s, flush=True))
    print(f'pictures: {len(made)} ready in {a.cache or cache_dir(a.game)}, {len(failed)} without one')


if __name__ == '__main__':
    main()
