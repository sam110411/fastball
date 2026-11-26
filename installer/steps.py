"""The setup's steps, and the bookkeeping that makes a fresh install, a resume and an update one rule.

    check    always: the setup folder, the PC (its CPU level, Vulkan), the game files and keys, free space
    exefs    the update's code (main, sdk, rtld, main.npdm) into <dest>/game/exefs, checked against its version's hashes
    romfs    the game's data (base + update, about 37 GB) into <dest>/game/romfs (tools/romfs_dump.py; resumable)
    build    lift the code to C (recomp/gen.py, with the version's code patches, tools/data/patches-<version>.txt) and
             compile it with the prebuilt runtime (tools/build.py) into mlb.exe
    equipment  optional: every Road to the Show equipment item, offline (tools/equipment); only shown when in play
    packages   optional: Road to the Show's Packages, offline (tools/packages, runtime/mlb/packages.c); the same
    finish   always: mlb.cfg, the shader translator, DLSS and FSR for the graphics card, shortcuts, a Ryujinx save (optional), clean up

fastball-install.json in the install folder records each finished step with a key: a fingerprint of what its output
depends on. A step whose recorded key matches and whose output is still there is skipped. So a cancelled setup resumes
where it stopped, and a newer release on an existing folder only rebuilds mlb.exe (its key holds the release version):
the game files are not needed again.

The port runs game versions 1.0.20 and 1.0.15 (layout.GAME_VERSIONS). The version comes from the update NSP picked, else
from the code already installed, else it is the newest. Another version on an existing folder extracts the code and the
data again (the old version's data is removed first: a file of the same name and size may hold other bytes) and
rebuilds. Nothing of the keys file is ever stored, logged or shown; only its path is passed
to the tools.

After TotK Recomp installer/steps.py and runner.py @ v0.4.0 (MIT).
"""
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import traceback
import urllib.request
from pathlib import Path

from . import detect
from .layout import GAME_VERSIONS

STATE = 'fastball-install.json'
LEGACY_STATE = 'mlbts-install.json'   # its name up to 0.1.5: read when there is no STATE, and replaced by it


def state_file(dest):
    """The install record of dest: STATE, or the older name when only that one is there."""
    p = Path(dest) / STATE
    old = Path(dest) / LEGACY_STATE
    return old if not p.is_file() and old.is_file() else p
GIB = 1 << 30
SPACE_SLACK = 2 * GIB          # mlb.exe, the shader and pipeline caches, saves
TEMP_BYTES = 2 * GIB           # the generated C (about 600 MB) and its objects (about 200 MB), with room

DLSS_DLL = 'nvngx_dlss.dll'    # NVIDIA's DLSS library: NVIDIA RTX SDK license, downloaded, never shipped
DLSS_VERSION = '310.9.1'
DLSS_URL = f'https://raw.githubusercontent.com/NVIDIA/DLSS/v{DLSS_VERSION}/lib/Windows_x86_64/rel/{DLSS_DLL}'
DLSS_SIZE = 58956912
DLSS_SHA256 = '3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983'
# AMD FSR 3.1 / 4 (runtime/gfx/vk_ffx.c): AMD's signed Vulkan runtime from the FSR SDK 2.4.0 (its licence allows
# redistribution, but like DLSS it is downloaded from AMD's repository, never shipped). The same revision as the API
# headers in runtime/third_party/ffx.
FFX_REVISION = '7539ae8b6568ad29ed78a753404351daf3b5cca7'
FFX_URL = (f'https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/{FFX_REVISION}'
           '/Kits/FidelityFX/signedbin/')
FFX_DLLS = (('amd_fidelityfx_loader_vk.dll', 39184,
             '38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9'),
            ('amd_fidelityfx_upscaler_vk.dll', 36135696,
             '4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7'))

PLAYER_CFG = """# mlb.cfg - settings of MLB The Show 26 (PC port), one KEY=VALUE per line. A line starting with # is ignored.
# Most settings are in the in-game menu (F1), which writes its changes into this file.
# Environment variables with the same name (MLB_...) win over this file.

# The sampling profiler is a developer tool; it costs frame time while it runs.
MLB_SAMPLER=off
"""


class InstallError(Exception):
    """A failure the player can act on: the message says what happened and what to do."""


class Cancelled(Exception):
    pass


def fingerprint(*parts):
    return hashlib.sha256('\0'.join(str(p) for p in parts).encode()).hexdigest()[:16]


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def now_utc():
    return datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')


def gib(n):
    return f'{n / GIB:.1f} GB'


class Context:
    def __init__(self, layout, dest, reporter, options):
        self.layout, self.dest, self.reporter, self.options = layout, dest, reporter, options
        self.temp = layout.temp / ('install-' + fingerprint(str(dest).lower())[:8])
        self.game = dest / 'game'
        self.expected = self.pick_version()
        self.state = self.load_state()

    @property
    def cpu(self):
        """The CPU level the game is built for here: 'v3' (AVX2), 'v2' (no AVX2) or None (too old to run it); --cpu v3
        or v2 picks it instead (mlb.exe still refuses a level the CPU lacks: runtime/mlb/cpucheck.c)."""
        forced = getattr(self.options, 'cpu', 'auto')
        return forced if forced in ('v3', 'v2') else detect.cpu_level(self.layout, self.reporter.log)

    def pick_version(self):
        """tools/data/expected-<version>.json of the game version this install builds: the picked update's, else the
        installed code's, else the newest."""
        known = {v: json.loads(self.layout.expected(v).read_text(encoding='utf-8')) for v in GAME_VERSIONS}
        for p in (self.options.update, self.options.base):   # the check step swaps them when picked the other way round
            info = detect.identify(p) if p and Path(p).is_file() else None
            if info and info['kind'] == 'update' and info['version'] in detect.UPDATE_VERSIONS:
                return known[detect.UPDATE_VERSIONS[info['version']]]
        main = self.game / 'exefs' / 'main'
        if main.is_file():
            h = sha256_file(main)
            for e in known.values():
                if e['exefs_sha256']['main'] == h:
                    return e
        return known[GAME_VERSIONS[0]]

    def romfs_version(self):
        """The update version the extracted data in game/romfs belongs to, None when there is none. Installs from
        before 1.0.20 support wrote no game/romfs.version: their data is 1.0.15's."""
        mark = self.game / 'romfs.version'
        if mark.is_file():
            try:
                return int(mark.read_text(encoding='utf-8').strip())
            except (OSError, ValueError):
                return -1
        return 983040 if (self.game / 'romfs').is_dir() else None

    def load_state(self):
        p = state_file(self.dest)
        if p.is_file():
            try:
                data = json.loads(p.read_text(encoding='utf-8'))
                if isinstance(data, dict) and isinstance(data.get('steps'), dict):
                    return data
            except (OSError, ValueError):
                pass
        return {'format': 1, 'release': None, 'steps': {}}

    def save_state(self):
        self.dest.mkdir(parents=True, exist_ok=True)
        p = self.dest / STATE
        tmp = p.with_name(STATE + '.tmp')
        tmp.write_text(json.dumps(self.state, indent=1) + '\n', encoding='utf-8')
        os.replace(tmp, p)
        (self.dest / LEGACY_STATE).unlink(missing_ok=True)   # now under the new name

    def run_tool(self, step, cmd, on_line=None):
        """Run a tool of the setup folder, its output to the log (and on_line), from the setup folder. A tool that fails
        raises InstallError with its last lines."""
        rep = self.reporter
        rep.log('run: ' + ' '.join(str(c) for c in cmd))
        flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
        tail = []
        p = subprocess.Popen([str(c) for c in cmd], cwd=self.layout.root, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, creationflags=flags)
        try:
            for raw in p.stdout:
                line = raw.decode('utf-8', 'replace').rstrip()
                if not line:
                    continue
                if not (on_line and on_line(line)):
                    rep.log('  ' + line)
                tail = (tail + [line])[-12:]
            code = p.wait()
        except KeyboardInterrupt:
            p.kill()
            p.wait()
            raise
        if code:
            name = next((Path(str(c)).name for c in cmd if str(c).endswith('.py')), Path(str(cmd[0])).name)
            raise InstallError(f'{name} failed (exit code {code}):\n' + '\n'.join(tail[-8:]))


class Step:
    id = ''
    title = ''
    always = False

    def applies(self, ctx):
        """Whether this setup run has the step at all (an optional one nobody asked for is not even listed)."""
        return True

    def key(self, ctx):
        return ''

    def current(self, ctx, record):
        return True

    def run(self, ctx):
        return {}


# ---- check ----------------------------------------------------------------------------------------------------------

class Check(Step):
    id, title, always = 'check', 'Check the PC and the game files', True

    def run(self, ctx):
        rep, lay, opt = ctx.reporter, ctx.layout, ctx.options
        missing = lay.missing()
        if missing:
            raise InstallError('The setup folder is incomplete (' + missing[0] + ' is missing). Unpack the whole '
                               'download again, into an empty folder.')
        if not ctx.cpu:
            raise InstallError('This CPU is too old for the game: it needs SSE4.2 and POPCNT (Intel Core i 1st '
                               'generation, AMD FX / Bulldozer or newer).')
        if ctx.cpu == 'v2':
            rep.log('check: this CPU has no AVX2: the game is built for x86-64-v2 (SSE4.2), which runs slower')
        if not detect.vulkan():
            raise InstallError('No Vulkan driver found (vulkan-1.dll). Install the latest driver for your graphics card.')
        # Portable: the game goes in a folder of its own inside the setup folder (MLBTS\ by default, as RecompLauncher
        # does), never in the setup folder itself or one of the setup's own folders, which an update replaces
        if lay.reserved(ctx.dest):
            raise InstallError(f'Pick an install folder of its own, such as {lay.default_dest()}: not the setup folder '
                               'or one of its folders.')
        needs_files = ctx.plan_runs('exefs') or ctx.plan_runs('romfs')
        if needs_files:
            self.check_files(ctx)
        else:
            rep.log('check: game files not needed, the extracted ones are complete')
        # space: what is left of the RomFS to write, plus the build's output in the install folder
        have = 0
        romfs = ctx.game / 'romfs'
        if ctx.plan_runs('romfs') and romfs.is_dir() and ctx.romfs_version() == ctx.expected['update_version']:
            have = sum(f.stat().st_size for f in romfs.rglob('*') if f.is_file() and not f.name.endswith('.part'))
        need = (max(ctx.expected['romfs_bytes'] - have, 0) if ctx.plan_runs('romfs') else 0) + SPACE_SLACK
        free = detect.free_bytes(ctx.dest)
        rep.log(f'space: install disk needs {gib(need)}, {gib(free)} free')
        if free < need:
            raise InstallError(f'Not enough free space for the install folder: it needs {gib(need)}, the disk has '
                               f'{gib(free)} free. Free some space or pick a folder on another disk.')
        if ctx.plan_runs('build'):
            tfree = detect.free_bytes(lay.temp)
            if tfree < TEMP_BYTES:
                raise InstallError(f'Not enough free space next to the setup for the build: it needs {gib(TEMP_BYTES)}'
                                   f', the disk has {gib(tfree)} free. Move the setup folder to a disk with more space.')
        return {}

    def check_files(self, ctx):
        opt = ctx.options
        for what, p in (('base game', opt.base), ('update', opt.update), ('keys', opt.keys)):
            if not p:
                raise InstallError(f'Pick the {what} file.')
            if not Path(p).is_file():
                raise InstallError(f'The {what} file was not found: {p}')
        ids = {}
        for name in ('base', 'update'):
            info = detect.identify(getattr(opt, name))
            if info is None:
                raise InstallError(f'{getattr(opt, name)} is not an NSP file. The setup takes the game and its update '
                                   'as NSP files dumped from your own Switch.')
            ids[name] = info
        if ids['base']['kind'] == 'update' and ids['update']['kind'] == 'base':   # picked the other way round
            opt.base, opt.update = opt.update, opt.base
            ids['base'], ids['update'] = ids['update'], ids['base']
            ctx.reporter.log('check: the base game and the update were swapped; using them the right way round')
        if ids['base']['kind'] != 'base':
            raise InstallError(f'{Path(opt.base).name} is not MLB The Show 26 (title {ids["base"]["title"]:016X}); '
                               'pick the base game NSP (title 0100AA7024094000).')
        if ids['update']['kind'] != 'update':
            raise InstallError(f'{Path(opt.update).name} is not an MLB The Show 26 update; pick the 1.0.20 update NSP '
                               '(title 0100AA7024094800, version 1310720) or the 1.0.15 one (version 983040).')
        ver = ids['update']['version']
        if ver is not None and ver not in detect.UPDATE_VERSIONS:
            raise InstallError(f'This update is version {ver}. The port runs on game versions 1.0.20 (1310720) and '
                               '1.0.15 (983040): dump one of those updates from your Switch.')
        text = Path(opt.keys).read_text(encoding='utf-8', errors='replace')
        if not re.search(r'^\s*header_key\s*=', text, re.M):
            raise InstallError(f'{opt.keys} does not look like a prod.keys file (it has no header_key).')
        if not opt.titlekeys:
            opt.titlekeys = detect.title_keys_for(opt.keys)


# ---- exefs ----------------------------------------------------------------------------------------------------------

class ExeFS(Step):
    id, title = 'exefs', "Extract the game's code"

    def key(self, ctx):
        return fingerprint('exefs-1', json.dumps(ctx.expected['exefs_sha256'], sort_keys=True))

    def current(self, ctx, record):
        ex = ctx.game / 'exefs'
        return all((ex / n).is_file() for n in ctx.expected['exefs_sha256'])

    def run(self, ctx):
        sys.path.insert(0, str(ctx.layout.tools))
        import nxextract as nx    # numpy and the Windows AES of swcrypto: the bundled Python has them
        from romfs_dump import open_program
        opt, rep = ctx.options, ctx.reporter
        rep.progress(self.id, 'Reading the update')
        keys = nx.load_keys(str(opt.keys))
        tk = nx.load_keys(str(opt.titlekeys)) if opt.titlekeys else {}
        try:
            nca, _ = open_program(str(opt.update), keys, tk)
        except (SystemExit, KeyError, ValueError) as e:
            raise InstallError(f'The update could not be opened with these keys ({e}). The keys must come from a '
                               'Switch whose system version is at least the one the update needs.') from None
        if nca.key is None:
            raise InstallError('The update could not be decrypted with these keys: dump prod.keys again from a '
                               'Switch on a recent system version.')
        sec = next((s for s in nca.sections if s['fs_type'] == 1 and s['index'] == 0), None)
        if sec is None:
            raise InstallError('The update holds no program code (ExeFS); it is not the '
                               f'{ctx.expected["display_version"]} update.')
        rd = nca.section_reader(sec)
        want = ctx.expected['exefs_sha256']
        out = ctx.game / 'exefs'
        out.mkdir(parents=True, exist_ok=True)
        got = {}
        for name, off, size in nx.parse_pfs0(rd, nca.pfs0_offset(sec)):
            data = rd(off, size)
            got[name] = hashlib.sha256(data).hexdigest()
            if name in want:
                tmp = out / (name + '.tmp')
                tmp.write_bytes(data)
                os.replace(tmp, out / name)
        bad = [n for n in want if got.get(n) != want[n]]
        if bad:
            v = ctx.expected['display_version']
            raise InstallError(f'The update\'s code is not the {v} code ({", ".join(bad)} differ). The port runs on game '
                               f'versions {" and ".join(GAME_VERSIONS)}: dump the update again from your Switch.')
        return {}


# ---- romfs ----------------------------------------------------------------------------------------------------------

class RomFS(Step):
    id, title = 'romfs', "Extract the game's data (about 37 GB)"

    def key(self, ctx):
        return fingerprint('romfs-1', ctx.expected['title_id'], ctx.expected['update_version'])

    def current(self, ctx, record):
        return ((ctx.game / 'romfs.complete').is_file() and (ctx.game / 'romfs').is_dir() and
                ctx.romfs_version() == ctx.expected['update_version'])

    def run(self, ctx):
        opt, lay, rep = ctx.options, ctx.layout, ctx.reporter
        ctx.game.mkdir(parents=True, exist_ok=True)
        want, have = ctx.expected['update_version'], ctx.romfs_version()
        if have is not None and have != want:
            # another version's data: a file of the same name and size may hold other bytes, and the extraction skips
            # files that are there at their size, so it starts from nothing
            rep.progress(self.id, "Removing the other game version's data", force=True)
            rep.log(f'romfs: the extracted data is version {have}, this install is {want}: removing it')
            (ctx.game / 'romfs.complete').unlink(missing_ok=True)
            shutil.rmtree(ctx.game / 'romfs', ignore_errors=True)
            if (ctx.game / 'romfs').exists():
                raise InstallError(f'{ctx.game / "romfs"} could not be removed (a file in it is in use): close the game '
                                   'and run the setup again.')
        (ctx.game / 'romfs.version').write_text(f'{want}\n', encoding='utf-8')
        cmd = [lay.python, '-I', '-X', 'utf8', lay.tools / 'romfs_dump.py', '--keys', opt.keys]
        if opt.titlekeys:
            cmd += ['--titlekeys', opt.titlekeys]
        cmd += [opt.base, opt.update, ctx.game, '--extract-all', '--progress']
        rep.progress(self.id, 'Checking the game files')
        t0, done0 = time.monotonic(), None

        def line(text):
            nonlocal done0
            m = re.match(r'progress (\d+) (\d+)$', text)
            if not m:
                return False
            done, total = int(m.group(1)), int(m.group(2))
            if done0 is None:
                done0 = done
            rate = (done - done0) / max(time.monotonic() - t0, 1e-3)
            left = f', about {int((total - done) / rate / 60) + 1} min left' if rate > 1e6 and done < total else ''
            rep.progress(self.id, f'{gib(done)} of {gib(total)}{left}', done, total, 'bytes')
            return True

        try:
            ctx.run_tool(self.id, cmd, line)
        except InstallError as e:
            if 'IVFC verify failed' in str(e):
                raise InstallError('The game files failed their integrity check: one of the NSP files is damaged or '
                                   'incomplete, or the base game and the update do not belong together. Copy or dump '
                                   'them again.') from None
            raise
        return {}


# ---- build ----------------------------------------------------------------------------------------------------------

class Build(Step):
    id, title = 'build', 'Build the game for this PC (a few minutes)'

    def key(self, ctx):
        patches = ctx.layout.patches(ctx.expected['display_version'])
        # the CPU level only when it is not the default, so v3 installs keep the key they had before the v2 build:
        # an exe built for v3 on a PC without AVX2 (an install moved over, or one of a setup up to 0.2.1) is built again
        return fingerprint('build-1', ctx.layout.release, json.dumps(ctx.expected['exefs_sha256'], sort_keys=True),
                           sha256_file(patches) if patches.is_file() else '',
                           *([] if ctx.cpu in ('v3', None) else ['cpu-' + ctx.cpu]))

    def current(self, ctx, record):
        return (ctx.dest / 'mlb.exe').is_file()

    def run(self, ctx):
        lay, rep = ctx.layout, ctx.reporter
        gen, out = ctx.temp / 'gen', ctx.temp / 'build'
        py = [lay.python, '-I', '-X', 'utf8']

        def lift_line(text):
            m = re.match(r'emitted (\d+)/(\d+) chunks', text)
            if m:
                rep.progress(self.id, 'Translating the code to C', int(m.group(1)), int(m.group(2)), 'chunks')
            return False

        rep.progress(self.id, 'Translating the code to C', force=True)
        # the code patches of this game version (tools/data/patches-<version>.txt); each set has a settings row
        patches = lay.patches(ctx.expected['display_version'])
        ctx.run_tool(self.id, py + [lay.recomp / 'gen.py', '--exefs', ctx.game / 'exefs', '--out', gen,
                                   '--modules', 'main,sdk'] + (['--patches', patches] if patches.is_file() else []),
                     lift_line)

        def build_line(text):
            m = re.match(r'\[(\d+)/(\d+)\]', text)
            if m:
                rep.progress(self.id, 'Compiling', int(m.group(1)), int(m.group(2)), 'files')
                return True
            return False

        rep.progress(self.id, 'Compiling', force=True)
        # the runtime library and the game code for the CPU's level (tools/build.py --cpu): v2 has no AVX2 and FMA
        cpu = ctx.cpu or 'v3'
        lib = lay.runtime_lib if cpu == 'v3' else lay.runtime_lib_v2
        if not lib.is_file():
            raise InstallError(f'The setup folder is incomplete ({lib} is missing). Unpack the whole download again, '
                               'into an empty folder.')
        ctx.run_tool(self.id, py + [lay.tools / 'build.py', '--toolchain', lay.llvm, '--deps', lay.deps,
                                   '--ninja', lay.ninja, '--runtime-lib', lib, '--cpu', cpu, '--portable',
                                   '--exefs', ctx.game / 'exefs', '--gen', gen, '--out', out], build_line)
        rep.progress(self.id, 'Copying the game into the install folder', force=True)
        for src in (out / 'mlb.exe', out / 'SDL3.dll', lay.xlate, lay.xlate.with_suffix('.id')):
            dst = ctx.dest / src.name
            tmp = dst.with_name(dst.name + '.new')
            shutil.copy2(src, tmp)
            try:
                os.replace(tmp, dst)
            except PermissionError:
                os.remove(tmp)
                raise InstallError(f'{dst} is in use: close the game and run the setup again.') from None
        return {'cpu': cpu}


# ---- equipment ------------------------------------------------------------------------------------------------------

class Equipment(Step):
    """Optional: every Road to the Show equipment item, offline (tools/equipment/equipment.py). --equipment adds it and
    --no-equipment takes it out again. Without either, an install keeps what it has: a later setup run (update,
    repair, or the game data extracted again) puts the equipment back when it is gone or this release is another."""
    id, title = 'equipment', 'Road to the Show equipment'
    MARK = '<!-- rtts-equipment start -->'   # what the tool leaves in the game's item catalog

    def wanted(self, ctx):
        opt = ctx.options
        if getattr(opt, 'no_equipment', False):
            return False
        return bool(getattr(opt, 'equipment', False) or (ctx.state['steps'].get(self.id) or {}).get('installed')
                    or Packages.wanted(ctx))

    def owned(self, ctx):
        """With the packages option the player earns the equipment from packs, so none of it is owned at the start."""
        return not Packages.wanted(ctx)

    def installed(self, ctx):
        catalog = ctx.game / 'romfs' / 'FRONTEND' / 'RTTS' / 'INVENTORYDATA.XML'
        try:
            return self.MARK in catalog.read_text(encoding='utf-8')
        except OSError:
            return False

    def applies(self, ctx):
        return self.wanted(ctx) or self.installed(ctx)

    def key(self, ctx):
        return fingerprint('equipment-1', str(self.wanted(ctx)), ctx.layout.release, ctx.expected['update_version'],
                           *([] if self.owned(ctx) else ['not owned']))

    def current(self, ctx, record):
        return self.installed(ctx) == self.wanted(ctx)

    def run(self, ctx):
        lay, rep = ctx.layout, ctx.reporter
        tool = [lay.python, '-I', '-X', 'utf8', lay.tools / 'equipment' / 'equipment.py']
        if not self.wanted(ctx):
            rep.progress(self.id, 'Putting the game files back', force=True)
            ctx.run_tool(self.id, [*tool, 'uninstall', '--game', ctx.dest])
            return {'installed': False}
        rep.progress(self.id, 'Adding the equipment', force=True)

        def line(text):
            m = re.match(r'pictures: (\d+) of (\d+)', text)
            if m:
                rep.progress(self.id, f'Making the item pictures from your game files: {m.group(1)} of {m.group(2)}',
                             int(m.group(1)), int(m.group(2)), 'items')
            return bool(m)

        ctx.run_tool(self.id, [*tool, 'install', '--game', ctx.dest, *([] if self.owned(ctx) else ['--no-own'])], line)
        return {'installed': True}


# ---- packages -------------------------------------------------------------------------------------------------------

def set_cfg(cfg, values):
    """Sets KEY=value lines in mlb.cfg (a value of None takes the key's lines out) and leaves every other line alone.
    A key may stand in the file as MLB_X, TOTK_X or X, as the game reads it (runtime/mlb/cfg.c)."""
    def name(key):
        key = key.strip().upper()
        return key[5:] if key.startswith('TOTK_') else key[4:] if key.startswith('MLB_') else key

    values = {name(k): (k, v) for k, v in values.items()}
    lines = cfg.read_text(encoding='utf-8').splitlines() if cfg.is_file() else PLAYER_CFG.splitlines()
    kept = [line for line in lines if not ('=' in line and not line.lstrip().startswith(('#', ';'))
                                           and name(line.split('=', 1)[0]) in values) and line != Packages.NOTE]
    add = [f'{k}={v}' for k, v in values.values() if v is not None]
    if add:
        while kept and not kept[-1].strip():
            kept.pop()
        kept += ['', Packages.NOTE, *add]
    while kept and not kept[-1].strip():
        kept.pop()
    tmp = cfg.with_name(cfg.name + '.tmp')
    tmp.write_text('\n'.join(kept) + '\n', encoding='utf-8')
    os.replace(tmp, cfg)


class Packages(Step):
    """Optional: Road to the Show's Packages without a connection. The game gets a small server on this PC to talk to
    (tools/packages; runtime/mlb/packages.c starts it with the game), which hands out the equipment packs the game
    awards and opens them. --packages turns it on, --no-packages off. It needs the equipment (the step above), which
    then starts not owned: the packs are how the player gets it. Like the equipment, an install that has it keeps it
    through updates and repairs.

    In the install folder: packages/ (the server and, from a release, the few files of the setup's Python it needs, so
    the game does not depend on where the setup folder is) and MLB_OFFLINE_PACKAGES=1 in mlb.cfg. What the player
    earned (profile/offline-packages) stays when the option is taken out."""
    id, title = 'packages', 'Road to the Show packages'
    NOTE = "# Road to the Show's Packages without a connection (the setup's option; see packages/README.md)"
    # what the server needs of the setup's Python: the interpreter, the standard library, TLS and the XML reader
    PYTHON = ('python.exe', 'python3??.dll', 'python3??.zip', 'vcruntime140*.dll', '_socket.pyd', '_ssl.pyd',
              'select.pyd', 'pyexpat.pyd', '_hashlib.pyd', 'unicodedata.pyd', 'libssl-*.dll', 'libcrypto-*.dll',
              'LICENSE.txt')

    @staticmethod
    def wanted(ctx):
        opt = ctx.options
        if getattr(opt, 'no_packages', False) or getattr(opt, 'no_equipment', False):
            return False
        return bool(getattr(opt, 'packages', False) or (ctx.state['steps'].get(Packages.id) or {}).get('installed'))

    def installed(self, ctx):
        try:
            cfg = (ctx.dest / 'mlb.cfg').read_text(encoding='utf-8')
        except OSError:
            return False
        return (ctx.dest / 'packages' / 'server.py').is_file() and bool(
            re.search(r'^\s*(MLB_|TOTK_)?OFFLINE_PACKAGES\s*=\s*1\s*$', cfg, re.M | re.I))

    def applies(self, ctx):
        return self.wanted(ctx) or self.installed(ctx)

    def key(self, ctx):
        return fingerprint('packages-1', str(self.wanted(ctx)), ctx.layout.release)

    def current(self, ctx, record):
        return self.installed(ctx) == self.wanted(ctx)

    def run(self, ctx):
        lay, rep, folder = ctx.layout, ctx.reporter, ctx.dest / 'packages'
        try:
            if folder.exists():
                shutil.rmtree(folder)
        except OSError as e:
            raise InstallError(f'{folder} is in use ({e}). Close the game and run the setup again.')
        if not self.wanted(ctx):
            rep.progress(self.id, 'Taking the packages option out', force=True)
            set_cfg(ctx.dest / 'mlb.cfg', {'MLB_OFFLINE_PACKAGES': None, 'MLB_PACKAGES_PYTHON': None})
            return {'installed': False}
        rep.progress(self.id, 'Adding the local packages server', force=True)
        source = lay.tools / 'packages'
        folder.mkdir(parents=True)
        for f in sorted(source.glob('*.py')) + [source / 'README.md']:
            shutil.copy2(f, folder / f.name)
        python = lay.python   # a checkout has no Python that can be copied: the game is told where the checkout's is
        if not lay.dev:
            python = folder / 'python' / 'python.exe'
            python.parent.mkdir()
            for pattern in self.PYTHON:
                found = sorted(lay.python.parent.glob(pattern))
                if not found:
                    raise InstallError(f'The setup folder is incomplete (python/{pattern} is missing). Unpack the '
                                       'whole download again, into an empty folder.')
                for f in found:
                    shutil.copy2(f, python.parent / f.name)
            library = next(python.parent.glob('python3??.zip'))
            (python.parent / (library.stem + '._pth')).write_text(library.name + '\n.\n', encoding='ascii')
        # the server once, as the game will run it: reads the item catalog and makes this PC's certificate
        rep.progress(self.id, 'Checking the server and making its certificate', force=True)
        ctx.run_tool(self.id, [python, '-I', '-X', 'utf8', folder / 'server.py', '--check', '--game', ctx.dest])
        set_cfg(ctx.dest / 'mlb.cfg', {'MLB_OFFLINE_PACKAGES': '1', 'MLB_PACKAGES_PYTHON': python if lay.dev else None})
        return {'installed': True}


# ---- finish ---------------------------------------------------------------------------------------------------------

class Finish(Step):
    id, title, always = 'finish', 'Finish', True

    def run(self, ctx):
        rep, opt, dest = ctx.reporter, ctx.options, ctx.dest
        cfg = dest / 'mlb.cfg'
        if not cfg.is_file():
            cfg.write_text(PLAYER_CFG, encoding='utf-8')
        self.upscalers(ctx)
        if opt.shortcut:
            rep.progress(self.id, 'Making shortcuts', force=True)
            # the launcher when there is one: it starts the game at once and checks for updates once a day
            launcher = ctx.layout.root / 'Fastball.exe'
            shortcuts(launcher if launcher.is_file() and not ctx.layout.dev else dest / 'mlb.exe', dest / 'mlb.exe')
        if opt.import_save:
            rep.progress(self.id, 'Copying your Ryujinx save', force=True)
            r = subprocess.run([str(dest / 'mlb.exe'), '--import-save'], cwd=dest, capture_output=True,
                               creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            text = (r.stdout + r.stderr).decode('utf-8', 'replace').strip()
            rep.log('import-save: ' + text.replace('\n', '\n  '))
            if r.returncode:
                rep.warning('Your Ryujinx save could not be copied (the setup log says why). The game starts with a '
                            'new save; you can copy it later with "Import Ryujinx save".')
        rep.progress(self.id, 'Cleaning up', force=True)
        shutil.rmtree(ctx.temp, ignore_errors=True)
        return {}

    def upscalers(self, ctx):
        """The upscalers' libraries for this PC's graphics card, without asking: NVIDIA DLSS on a GeForce RTX, AMD FSR
        (3.1 everywhere, 4 on the Radeons that run it) on any AMD, NVIDIA or Intel card. Each runs only once it is
        turned on in the game's settings (F1, Graphics), so a library on a PC that never uses it costs only disk space.
        Every setup and update looks again, so a new graphics card gets its library at the next update. --dlss / --fsr
        download one whatever the card; --no-dlss / --no-fsr never."""
        rep, opt = ctx.reporter, ctx.options
        names = detect.gpus()
        rep.log('graphics cards: ' + (', '.join(names) if names else 'none found in the registry'))
        if opt.dlss or (detect.nvidia_rtx(names) and not opt.no_dlss):
            self.download(ctx, DLSS_DLL, DLSS_URL, DLSS_SIZE, DLSS_SHA256, 'NVIDIA DLSS', 'NVIDIA RTX SDK license')
        if opt.fsr or (detect.fsr_gpu(names) and not opt.no_fsr):
            for name, size, sha in FFX_DLLS:
                if not self.download(ctx, name, FFX_URL + name, size, sha, 'AMD FSR', 'AMD FSR SDK license'):
                    break

    def download(self, ctx, name, url, size, sha256, label, licence):
        """url into the install folder as name, checked by size and SHA-256. False when it could not be had (a
        warning: the game runs without that upscaler)."""
        rep, target = ctx.reporter, ctx.dest / name
        if target.is_file():
            return True   # this setup's copy, or a newer one the player put there
        part = ctx.temp / (name + '.part')
        rep.log(f'{label}: {url} ({licence})')
        try:
            part.parent.mkdir(parents=True, exist_ok=True)
            h, done = hashlib.sha256(), 0
            with urllib.request.urlopen(url, timeout=60) as r, open(part, 'wb') as f:
                while block := r.read(1 << 20):
                    f.write(block)
                    h.update(block)
                    done += len(block)
                    rep.progress(self.id, f'Downloading {label}', min(done, size), size, 'bytes')
            if done != size or h.hexdigest() != sha256:
                raise OSError(f'{done} bytes, not the expected file')
            os.replace(part, target)
            return True
        except Exception as e:   # urllib's errors, a cut connection, a bad file: the game runs without it
            if isinstance(e, KeyboardInterrupt):
                raise
            part.unlink(missing_ok=True)
            rep.warning(f'{label} could not be downloaded ({str(e)[:200]}). The game runs without it; run the '
                        'setup again to retry.')
            return False


def shortcuts(exe, icon):
    """'MLB The Show 26' on the desktop and in the Start menu, through the Windows shell (WScript.Shell). Only when the
    player ticks the option (off by default: the port is portable and keeps to its folder)."""
    script = ("$w = New-Object -ComObject WScript.Shell; "
              "foreach ($d in @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('Programs'))) { "
              "$s = $w.CreateShortcut((Join-Path $d 'MLB The Show 26.lnk')); $s.TargetPath = $env:FASTBALL_EXE; "
              "$s.WorkingDirectory = (Split-Path $env:FASTBALL_EXE); $s.IconLocation = $env:FASTBALL_ICON + ',0'; "
              "$s.Description = 'MLB The Show 26 (Fastball)'; $s.Save() }")
    subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', script],
                   env=dict(os.environ, FASTBALL_EXE=str(exe), FASTBALL_ICON=str(icon)), capture_output=True,
                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)


STEPS = (Check, ExeFS, RomFS, Build, Equipment, Packages, Finish)


def all_steps():
    return [s() for s in STEPS]


def plan(ctx, steps):
    """Which steps run: always ones, and those without a matching record or whose output is gone."""
    ctx.keys = {s.id: s.key(ctx) for s in steps}
    runs = {}
    for s in steps:
        rec = ctx.state['steps'].get(s.id)
        runs[s.id] = s.always or not (rec and rec.get('key') == ctx.keys[s.id] and s.current(ctx, rec))
    if runs['exefs']:   # new code means a new build
        runs['build'] = True
    ctx.plan_runs = lambda step_id: runs.get(step_id, False)
    return runs


def run_steps(ctx, steps):
    """Run the plan. Returns 'ok', 'failed' or 'cancelled'; the events and the log say why."""
    rep, current = ctx.reporter, None
    try:
        runs = plan(ctx, steps)
        for s in steps:
            if not runs[s.id]:
                rep.step(s.id, 'skipped', reason='already done')
                continue
            current = s.id
            rep.step(s.id, 'started')
            t0 = rep.elapsed()
            fields = s.run(ctx) or {}
            seconds = round(rep.elapsed() - t0, 1)
            if not s.always:
                ctx.state['steps'][s.id] = {'key': ctx.keys[s.id], 'release': ctx.layout.release,
                                            'finished': now_utc(), 'seconds': seconds, **fields}
                ctx.save_state()
            rep.step(s.id, 'done', seconds=seconds)
            current = None
        ctx.state['release'] = ctx.layout.release
        ctx.save_state()
        return 'ok'
    except InstallError as e:
        rep.error(str(e), step=current)
        return 'failed'
    except KeyboardInterrupt:
        rep.log(f'cancelled during {current}')
        return 'cancelled'
    except Exception as e:
        rep.log(traceback.format_exc())
        rep.error(f'Unexpected error: {type(e).__name__}: {e}. The setup log has the details.', step=current)
        return 'failed'
