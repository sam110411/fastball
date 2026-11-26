"""Finding what the player already has, so the setup window starts filled in: the keys, the game's NSP files, a Ryujinx
save, the GPU, free space. Standard library only; nothing here decrypts or reads key values.

An NSP is told apart without keys by its PFS0 entry names: the plain-text <id>.cnmt.xml (Type Application or Patch, the
title id, the version), else the ticket's name (<rights id>.tik, which starts with the title id).
"""
import ctypes
import json
import os
import re
import shutil
import struct
import subprocess
from pathlib import Path

TITLE_BASE = 0x0100AA7024094000      # MLB The Show 26, the base game
TITLE_UPDATE = TITLE_BASE + 0x800    # its update
UPDATE_VERSIONS = {1310720: '1.0.20', 983040: '1.0.15'}   # the updates the port runs: CNMT version -> game version
NEWEST_UPDATE = max(UPDATE_VERSIONS)

APPDATA = Path(os.environ.get('APPDATA') or Path.home() / 'AppData' / 'Roaming')
# Emulators keep the player's keys here; the setup reads the file it is given and nothing else of these folders
EMULATORS = ('Ryujinx', 'yuzu', 'suyu', 'sudachi', 'citron', 'eden', 'torzu')


def keys_candidates():
    out = [APPDATA / 'Ryujinx' / 'system' / 'prod.keys', Path.home() / '.switch' / 'prod.keys']
    out += [APPDATA / e / 'keys' / 'prod.keys' for e in EMULATORS[1:]]
    return out


def find_keys(extra_dirs=()):
    """The first prod.keys found: next to the setup or the game files first, then the emulators' folders."""
    for d in extra_dirs:
        p = Path(d) / 'prod.keys'
        if p.is_file():
            return p
    for p in keys_candidates():
        if p.is_file():
            return p
    return None


def title_keys_for(keys):
    """title.keys beside prod.keys, when there is one (the NSPs' own tickets usually make it unnecessary)."""
    p = Path(keys).with_name('title.keys') if keys else None
    return p if p and p.is_file() else None


def pfs0_names(path):
    """[(name, offset, size)] of an NSP's PFS0, or None for another kind of file."""
    try:
        with open(path, 'rb') as f:
            hdr = f.read(16)
            if len(hdr) < 16 or hdr[:4] != b'PFS0':
                return None
            count, strsz = struct.unpack_from('<II', hdr, 4)
            if count > 4096 or strsz > 1 << 20:
                return None
            raw = f.read(count * 0x18 + strsz)
            strtab = raw[count * 0x18:]
            base = 16 + count * 0x18 + strsz
            out = []
            for i in range(count):
                off, size, noff = struct.unpack_from('<QQI', raw, i * 0x18)
                end = strtab.index(b'\0', noff)
                out.append((strtab[noff:end].decode('utf-8', 'replace'), base + off, size))
            return out
    except (OSError, ValueError, struct.error):
        return None


def identify(path):
    """{'kind': 'base'|'update'|'other', 'title': int, 'version': int|None} of an NSP, or None when unreadable."""
    entries = pfs0_names(path)
    if entries is None:
        return None
    for name, off, size in entries:
        if name.endswith('.cnmt.xml') and size < 1 << 20:
            try:
                with open(path, 'rb') as f:
                    f.seek(off)
                    text = f.read(size).decode('utf-8', 'replace')
            except OSError:
                return None
            kind = re.search(r'<Type>(\w+)</Type>', text)
            tid = re.search(r'<Id>0x([0-9a-fA-F]{16})</Id>', text)
            ver = re.search(r'<Version>(\d+)</Version>', text)
            if kind and tid:
                return classify(int(tid.group(1), 16), int(ver.group(1)) if ver else None)
    for name, _, _ in entries:   # no cnmt.xml: the ticket's rights id starts with the title id
        if name.endswith('.tik') and len(name) >= 36:
            try:
                return classify(int(name[:16], 16), None)
            except ValueError:
                pass
    return {'kind': 'other', 'title': 0, 'version': None}


def classify(title, version):
    kind = 'base' if title == TITLE_BASE else 'update' if title == TITLE_UPDATE else 'other'
    return {'kind': kind, 'title': title, 'version': version}


def game_dirs(extra=()):
    """Folders where game files usually are: the ones given, the emulators' game lists, Downloads, Desktop."""
    dirs = [Path(d) for d in extra]
    try:
        cfg = json.loads((APPDATA / 'Ryujinx' / 'Config.json').read_text(encoding='utf-8'))
        dirs += [Path(d) for d in cfg.get('game_dirs', []) if isinstance(d, str)]
    except (OSError, ValueError):
        pass
    for e in EMULATORS[1:]:
        try:
            text = (APPDATA / e / 'config' / 'qt-config.ini').read_text(encoding='utf-8', errors='replace')
            dirs += [Path(m.strip()) for m in re.findall(r'^Paths\\gamedirs\\\d+\\path=(.+)$', text, re.M)]
        except OSError:
            pass
    home = Path.home()
    dirs += [home / 'Downloads', home / 'Desktop', home / 'Documents', home / 'Games']
    seen, out = set(), []
    for d in dirs:
        key = str(d).lower()
        if key not in seen and d.is_dir():
            seen.add(key)
            out.append(d)
    return out


def find_game_files(extra_dirs=(), depth=2):
    """{'base': Path|None, 'update': Path|None}: the MLB The Show 26 NSPs found in game_dirs, the update preferring
    the newest version the port runs (1.0.20, then 1.0.15). Looks depth folder levels deep, NSP and NSZ names only (it reads 16 bytes plus the entry table of each)."""
    found = {'base': None, 'update': None}
    update_ver = None

    def rank(v):   # a version the port runs over any other, then the newer
        return (v in UPDATE_VERSIONS, v or 0)

    def visit(d, level):
        nonlocal update_ver
        try:
            items = sorted(os.scandir(d), key=lambda e: e.name.lower())
        except OSError:
            return
        for e in items:
            try:
                if e.is_file() and e.name.lower().endswith('.nsp'):
                    info = identify(e.path)
                    if not info:
                        continue
                    if info['kind'] == 'base' and not found['base']:
                        found['base'] = Path(e.path)
                    elif info['kind'] == 'update' and (not found['update'] or rank(info['version']) > rank(update_ver)):
                        found['update'], update_ver = Path(e.path), info['version']
                elif e.is_dir() and level < depth and not e.name.startswith(('.', '$')):
                    visit(e.path, level + 1)
            except OSError:
                continue

    for d in game_dirs(extra_dirs):
        visit(d, 0)
        if found['base'] and update_ver == NEWEST_UPDATE:
            break
    return found


def ryujinx_save():
    """True when Ryujinx has a save of this game (mlb.exe --import-save finds and copies it)."""
    save_root = APPDATA / 'Ryujinx' / 'bis' / 'user' / 'save'
    if not save_root.is_dir():
        return False
    # imkvdb.arc maps (title, user) to a save folder number; the title id appears in it little-endian
    try:
        data = (APPDATA / 'Ryujinx' / 'bis' / 'system' / 'save' / '8000000000000000' / '0' / 'imkvdb.arc').read_bytes()
    except OSError:
        return False
    return struct.pack('<Q', TITLE_BASE) in data


def gpus():
    """The display adapters' names from the registry (DriverDesc of the display class)."""
    names = []
    try:
        import winreg
        cls = r'SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, cls) as k:
            for i in range(64):
                try:
                    sub = winreg.EnumKey(k, i)
                except OSError:
                    break
                try:
                    with winreg.OpenKey(k, sub) as s:
                        names.append(str(winreg.QueryValueEx(s, 'DriverDesc')[0]))
                except OSError:
                    continue
    except ImportError:
        pass
    return names


def nvidia_rtx(names=None):
    """An NVIDIA RTX GPU: the GPUs DLSS runs on."""
    return any('NVIDIA' in n.upper() and 'RTX' in n.upper() for n in (gpus() if names is None else names))


def fsr_gpu(names=None):
    """An AMD, NVIDIA or Intel GPU: AMD FSR 3.1 runs on every one the game runs on (FSR 4 on the Radeons AMD made it
    for, which AMD's runtime picks itself). Not a basic display adapter or a remote-desktop one."""
    return any(v in n.upper() for n in (gpus() if names is None else names)
               for v in ('AMD', 'RADEON', 'NVIDIA', 'INTEL'))


_cpu = {}
# The CPU probe: printed level of runtime/core/cpulevel.h (the tests the runtime's start check uses too). Python cannot
# read CPUID, and IsProcessorFeaturePresent knows AVX2 but not FMA, BMI2, F16C, LZCNT or MOVBE, which v3 code uses as
# well. Kept here, not as a .c file: a release holds no C source outside its toolchain (make_release.py check).
CPU_PROBE = '#include <stdio.h>\n#include "cpulevel.h"\nint main(void) { printf("%d\\n", cpulevel()); return 0; }\n'


def cpu_level(layout, log=print):
    """The x86-64 level the game is built for on this PC: 'v3' (AVX2, FMA, BMI2: the default, faster build), 'v2'
    (SSE4.2, POPCNT: CPUs without AVX2) or None (too old). From CPUID, by CPU_PROBE compiled with the setup's clang
    for baseline x86-64 into the temp folder (about 2 s, once per run). If that cannot run, IsProcessorFeaturePresent
    decides, which knows AVX2 and SSE4.2 but not the rest of each level."""
    if 'level' in _cpu:
        return _cpu['level']
    level = None
    probe = Path(layout.temp) / 'cpuprobe.exe'
    src = probe.with_suffix('.c')
    try:
        probe.parent.mkdir(parents=True, exist_ok=True)
        src.write_text(CPU_PROBE, encoding='ascii')
        flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
        subprocess.run([str(Path(layout.llvm) / 'bin' / 'clang.exe'), '-O1', '-march=x86-64', '-I',
                        str(Path(layout.root) / 'runtime' / 'core'), str(src), '-o', str(probe)], check=True,
                       capture_output=True, timeout=120, creationflags=flags)
        out = subprocess.run([str(probe)], check=True, capture_output=True, text=True, timeout=30,
                             creationflags=flags).stdout.strip()
        level = {'3': 'v3', '2': 'v2', '0': None}[out]
        log(f'cpu: x86-64-{level or "v1 (too old)"} (CPUID)')
    except (OSError, subprocess.SubprocessError, KeyError) as e:
        try:
            k = ctypes.windll.kernel32
            # PF_AVX2_INSTRUCTIONS_AVAILABLE, PF_SSE4_2_INSTRUCTIONS_AVAILABLE
            level = 'v3' if k.IsProcessorFeaturePresent(40) else 'v2' if k.IsProcessorFeaturePresent(38) else None
        except (AttributeError, OSError):
            level = 'v3'
        log(f'cpu: probe failed ({type(e).__name__}: {e}); IsProcessorFeaturePresent says x86-64-{level or "v1"}')
    _cpu['level'] = level
    return level


def vulkan():
    system = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32'
    return (system / 'vulkan-1.dll').is_file()


def free_bytes(path):
    """Free space on the disk that holds path (or its nearest existing parent)."""
    p = Path(path)
    while not p.exists() and p.parent != p:
        p = p.parent
    try:
        return shutil.disk_usage(p).free
    except OSError:
        return 0


def default_dest():
    """The suggested install folder: Games\\MLB The Show 26 on the fixed disk with the most free space."""
    best, best_free = None, -1
    for letter in 'CDEFGHIJKLMNOPQRSTUVWXYZ':
        root = Path(f'{letter}:\\')
        try:
            if ctypes.windll.kernel32.GetDriveTypeW(str(root)) != 3:   # DRIVE_FIXED
                continue
        except (AttributeError, OSError):
            if letter != 'C':
                continue
        free = free_bytes(root)
        if free > best_free:
            best, best_free = root, free
    return (best or Path('C:\\')) / 'Games' / 'MLB The Show 26'
