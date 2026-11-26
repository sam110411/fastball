"""Where the setup finds what it runs: the one place that maps the release folder and a repository checkout.

Release (docs/release.md, "Release layout"), root = the unpacked folder:

    python/python.exe     runs every tool (numpy, lz4, tkinter); its ._pth puts .., ../tools and ../recomp on sys.path
    llvm/                 llvm-mingw: clang, lld, llvm-ar, llvm-windres and the mingw-w64 headers and libraries
    runtime/              libmlbrt.a and libmlbrt-v2.a (the runtime for CPUs with and without AVX2, no game code),
                          the headers the game code and modules.c include,
                          runtime/deps (Vulkan, VMA and SDL3 headers, lib/vulkan-1.lib, bin/SDL3.dll)
    bin/                  ninja.exe, totk_xlate.exe and totk_xlate.id (the shader translator)
    recomp/, tools/       the lifter and the extraction and build scripts; tools/equipment and tools/packages (the
                          optional setup steps)
    VERSION               the release version, one line
    temp/                 the default temp folder (created)
    logs/                 the setup's logs (created)
    launcher.json         the launcher's memory: the install folder, the file paths, the last update check (created)
    MLBTS/                the default install folder (portable: everything stays in the folder the player unpacked;
                          RecompLauncher installs there too, recomplauncher.json)

Dev mode, root = a repository checkout (no python/python.exe): the venv's Python, work/toolchain/llvm-mingw-*,
work/rtlib/libmlbrt.a (tools/build.py --toolchain ... --make-runtime-lib --out work/rtlib; libmlbrt-v2.a with --cpu v2,
only needed to test a v2 setup), work/deps, the venv's ninja,
work/xlate and work/install-temp, work/launcher.json and work/logs. Its version is VERSION plus -dev.
"""
import os
import sys
from dataclasses import dataclass
from pathlib import Path

# What a release folder holds (tools/make_release.py); an update replaces these and nothing else. UPDATE_DIR: where
# the launcher unpacks a new release before it swaps it in (update.py).
RELEASE_ENTRIES = ('Fastball.exe', 'python', 'llvm', 'runtime', 'bin', 'recomp', 'tools', 'installer', 'licenses',
                   'LICENSE', 'THIRD_PARTY_NOTICES.md', 'README.txt', 'VERSION', 'recomplauncher.json')
UPDATE_DIR = '.update'
# What older releases held and this one no longer does: an in-place update removes them (update.apply), and the window
# removes them from a folder an older updater unpacked (gui.py). MLBTS-Setup.exe: the launcher up to 0.1.5.
LEGACY_ENTRIES = ('MLBTS-Setup.exe',)

# The game versions the port builds, newest first: each has tools/data/expected-<version>.json (ExeFS hashes, RomFS
# size). A setup builds the version of the update NSP it is given (steps.Context.pick_version).
GAME_VERSIONS = ('1.0.20', '1.0.15')


@dataclass(frozen=True)
class Layout:
    root: Path
    dev: bool
    release: str
    python: Path
    tools: Path
    recomp: Path
    llvm: Path
    deps: Path
    runtime_lib: Path    # libmlbrt.a: the runtime for x86-64-v3 (AVX2), the default
    runtime_lib_v2: Path  # libmlbrt-v2.a: for CPUs without AVX2 (tools/build.py --cpu v2)
    ninja: Path
    xlate: Path          # totk_xlate.exe; the totk_xlate.id beside it is copied too
    temp: Path
    state: Path          # launcher.json

    @property
    def logs(self):
        """The setup's logs: logs in the setup folder (a repository: work/logs)."""
        return self.state.parent / 'logs'

    def default_dest(self):
        """The portable install folder: MLBTS inside the setup folder (a repository: work/install)."""
        return self.root / ('work/install' if self.dev else 'MLBTS')

    def reserved(self, dest):
        """dest is the setup folder itself or one of the setup's own folders (or inside one), which an update
        replaces: no place for the game. A folder of its own inside the setup folder (MLBTS/) is fine."""
        try:
            d = Path(dest).resolve()
        except OSError:
            return True
        if self.dev:
            return d == self.root
        own = [self.root / n for n in RELEASE_ENTRIES] + [self.temp, self.logs, self.root / UPDATE_DIR]
        return d == self.root or any(d == o or o in d.parents for o in own)

    def expected(self, version):
        return self.tools / 'data' / f'expected-{version}.json'

    def patches(self, version):
        """The code patches for that game version (recomp/patches.py format), passed to recomp/gen.py --patches."""
        return self.tools / 'data' / f'patches-{version}.txt'

    def missing(self):
        """Paths the steps need that do not exist."""
        need = [self.python, self.tools / 'build.py', self.tools / 'romfs_dump.py', self.tools / 'nxextract.py',
                self.tools / 'equipment' / 'equipment.py', self.tools / 'packages' / 'server.py',
                self.recomp / 'gen.py', self.llvm / 'bin' / 'clang.exe', self.deps / 'lib' / 'vulkan-1.lib',
                self.deps / 'bin' / 'SDL3.dll', self.runtime_lib, self.ninja, self.xlate]
        if not self.dev:   # a repository builds work/rtlib/libmlbrt-v2.a only to test the v2 setup
            need.append(self.runtime_lib_v2)
        need += [self.expected(v) for v in GAME_VERSIONS] + [self.patches(v) for v in GAME_VERSIONS]
        return [str(p) for p in need if not p.exists()]


def release_layout(root):
    version = root / 'VERSION'
    return Layout(root=root, dev=False,
                  release=version.read_text(encoding='utf-8').strip() if version.is_file() else 'unknown',
                  python=root / 'python' / 'python.exe', tools=root / 'tools', recomp=root / 'recomp',
                  llvm=root / 'llvm', deps=root / 'runtime' / 'deps', runtime_lib=root / 'runtime' / 'libmlbrt.a',
                  runtime_lib_v2=root / 'runtime' / 'libmlbrt-v2.a',
                  ninja=root / 'bin' / 'ninja.exe', xlate=root / 'bin' / 'totk_xlate.exe', temp=root / 'temp',
                  state=root / 'launcher.json')


def dev_layout(root):
    venv = root / '.venv' / 'Scripts'
    trees = sorted((root / 'work' / 'toolchain').glob('llvm-mingw-*-ucrt-x86_64'))
    return Layout(root=root, dev=True, release=(root / 'VERSION').read_text(encoding='utf-8').strip() + '-dev',
                  python=venv / 'python.exe' if (venv / 'python.exe').is_file() else Path(sys.executable),
                  tools=root / 'tools', recomp=root / 'recomp',
                  llvm=trees[-1] if trees else root / 'work' / 'toolchain' / 'llvm-mingw-missing',
                  deps=root / 'work' / 'deps', runtime_lib=root / 'work' / 'rtlib' / 'libmlbrt.a',
                  runtime_lib_v2=root / 'work' / 'rtlib' / 'libmlbrt-v2.a',
                  ninja=venv / 'ninja.exe', xlate=root / 'work' / 'xlate' / 'totk_xlate.exe',
                  temp=root / 'work' / 'install-temp', state=root / 'work' / 'launcher.json')


def find(root=None):
    """The layout of root (default: the folder above this package)."""
    root = Path(root or Path(__file__).resolve().parent.parent).resolve()
    return release_layout(root) if (root / 'python' / 'python.exe').is_file() else dev_layout(root)


def legacy_settings():
    """%LOCALAPPDATA%\\MLBTS-Native\\setup.json: where setups up to 0.1.5 remembered the install. A newer setup without
    a launcher.json reads it once (gui.load_settings) and removes it: nothing of the port stays outside its folders."""
    base = os.environ.get('LOCALAPPDATA') or str(Path.home() / 'AppData' / 'Local')
    return Path(base) / 'MLBTS-Native' / 'setup.json'
