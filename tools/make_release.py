"""Assemble the setup release of docs/release.md from this checkout and pinned downloads, and zip it.

    .venv/Scripts/python -I tools/make_release.py [--out work/release] [--no-zip] [--from-worktree]

Writes work/release/Fastball-<version>/ and Fastball-<version>-windows.zip next to it, from a clean export of the
last commit (git archive HEAD into work/release-src): uncommitted changes are never in a release, and the working
tree is not touched. --from-worktree builds from the working tree instead, for tests. Needs network access on the
first run: downloads are cached in work/toolchain/release-dl and checked against the SHA-256 in PINS (TotK Recomp's
pins for the same files); a mismatch stops the build. Also needs, built beforehand:

    work/deps         tools/fetch_deps.py (Vulkan headers, VMA, SDL3, vulkan-1.lib)
    work/xlate        tools/build_xlate.py (the shader translator; its identity must match the sources)
    work/gen          recomp/gen.py: only its shared headers (layout.h, patches.h) and the button icons are read, to
                      compile the runtime library; none of the generated C goes into the release

The folder:

    Fastball.exe      installer/launcher/launcher.c built with llvm/: starts python/pythonw.exe -m installer.gui
    python/           python.org embeddable 3.14.9, numpy and lz4 (tools/requirements-release.txt), and tkinter (the
                      Tcl/Tk files come out of the python.org installer's own component MSI, unpacked with msiexec /a;
                      the installer is never run)
    llvm/             llvm-mingw 20261020, x86_64 only: what clang and lld need to compile and link mlb.exe
    runtime/          libmlbrt.a and libmlbrt-v2.a (tools/build.py --make-runtime-lib with llvm/, --cpu v3 and v2:
                      the runtime for CPUs with and without AVX2, no game code), every
                      runtime header, deps/ (include/, lib/vulkan-1.lib, bin/SDL3.dll)
    bin/              ninja.exe, totk_xlate.exe and totk_xlate.id
    recomp/, tools/, installer/, licenses/, LICENSE, THIRD_PARTY_NOTICES.md, README.txt
    VERSION           VERSION plus the commit (0.1.0+507213f): the setup's release string

The release contains no code, data or assets of the game, and no file built from them: no mlb.exe, no generated C, no
objects of it, no shader translations. Before zipping, every file is checked (check_content's magics, generated-code
marker and key lines; and no executable but the listed tools), then the folder's own Python imports what the setup
needs, the window builds every screen (--selftest), and a setup run without game files must stop at the missing files
after passing the folder check.

Also MLBTS-Setup-<version>-windows.zip: the same folder plus a copy of Fastball.exe named MLBTS-Setup.exe, for the
updaters of setups up to 0.1.5, which look only for that zip and start only that exe (installer/update.py). Publish both.
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import build_xlate  # noqa: E402
import check_content  # noqa: E402

SRC = ROOT   # the sources the release is made of: a clean export of HEAD (main() sets it), or ROOT with --from-worktree
DL = ROOT / 'work' / 'toolchain' / 'release-dl'
WORK = ROOT / 'work' / 'toolchain' / 'release-work'
LLVM_VERSION = '20261020'
PY_VERSION = '3.14.9'
# name: (url, sha256); the same files and hashes TotK Recomp's tools/make_release.py pins
PINS = {
    'python-embed': (f'https://www.python.org/ftp/python/{PY_VERSION}/python-{PY_VERSION}-embed-amd64.zip',
                     '3c01d7cfc666f746a6f064f60fb2156ab0ec464d3d738ab796371a46a9bb24a2'),
    'python-installer': (f'https://www.python.org/ftp/python/{PY_VERSION}/python-{PY_VERSION}-amd64.exe',
                         'a247a0b612244a803f507b39fb7121e3602e4160d59c3b99d98e735289c53394'),
    'llvm-mingw': (f'https://github.com/mstorsjo/llvm-mingw/releases/download/{LLVM_VERSION}/'
                   f'llvm-mingw-{LLVM_VERSION}-ucrt-x86_64.zip',
                   'fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd'),
    'ninja': ('https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip',
              '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'),
}
# The component MSI inside the python.org installer that carries Tcl/Tk and tkinter (Burn names its payloads a0..a27)
TCLTK_PAYLOAD = ('a12', 'ac87609db3f715404dd41a989c2f1706ea1512f3d86217f938d11c63af8eb505')
# llvm-mingw: what clang and lld need to compile and link mlb.exe for x86_64-w64-mingw32 (TotK Recomp's list)
LLVM_BIN = {'clang.exe', 'clang++.exe', 'clang-23.exe', 'ld.lld.exe', 'llvm-ar.exe', 'llvm-ranlib.exe', 'llvm-nm.exe',
            'llvm-objdump.exe', 'llvm-objcopy.exe', 'llvm-strip.exe', 'llvm-readobj.exe', 'llvm-dlltool.exe',
            'llvm-rc.exe', 'llvm-cvtres.exe', 'llvm-windres.exe', 'llvm-wrapper.exe', 'mingw32-common.cfg',
            'x86_64-w64-windows-gnu.cfg', 'x86_64-pc-windows-gnu.cfg', 'x86_64-w64-mingw32-clang.exe',
            'x86_64-w64-mingw32-clang++.exe', 'libLLVM-23.dll', 'libclang-cpp.dll', 'libc++.dll', 'libunwind.dll',
            'libwinpthread-1.dll', 'libffi-8.dll'}
LLVM_SKIP_DIRS = {'aarch64-w64-mingw32', 'armv7-w64-mingw32', 'i686-w64-mingw32', 'busybox', 'python', 'share'}
# tools/ the setup runs (and what they import)
TOOLS_FILES = ['build.py', 'check_content.py', 'romfs_dump.py', 'nxextract.py', 'ncastorage.py', 'swcrypto.py',
               'hostmem.py', 'data/expected-1.0.15.json', 'data/expected-1.0.20.json', 'data/patches-1.0.15.txt',
               'data/patches-1.0.20.txt']   # hostmem: recomp/gen.py sizes its workers with it
# Every script the setup runs: smoke() starts each with --help on the folder's own Python, so a missing import stops
# the release instead of a player's install (recomp/gen.py's hostmem did, after the 37 GB extraction)
SETUP_SCRIPTS = ['recomp/gen.py', 'tools/build.py', 'tools/romfs_dump.py', 'tools/equipment/equipment.py',
                 'tools/packages/server.py']
# The only executables and libraries a release may hold (folders are prefixes); anything else stops the release
EXE_ALLOWED = ('Fastball.exe', 'python/', 'llvm/', 'bin/ninja.exe', 'bin/totk_xlate.exe', 'runtime/deps/bin/SDL3.dll',
               'runtime/deps/lib/vulkan-1.lib', 'runtime/libmlbrt.a', 'runtime/libmlbrt-v2.a')
README = """Fastball {version}: MLB The Show 26 for PC

Start Fastball.exe. It builds the game on this PC from your own game files, into the MLBTS folder here (or a
folder you pick). Everything stays in this folder: delete it and the port is gone.

You need:
- MLB The Show 26 (NSP) and its 1.0.20 update (NSP; the 1.0.15 update works too), dumped from your own Nintendo
  Switch;
- prod.keys from your own Switch (the setup finds it in Ryujinx's or yuzu's folder by itself);
- about 40 GB free, Windows 10 or 11 (64-bit), a 64-bit CPU with SSE4.2 (AVX2 recommended: without it the game
  is built for your CPU too, and runs slower) and a graphics card with Vulkan 1.3.

No game files, keys or code of the game are in this download, and the setup never uploads anything. The game is
built on your PC, from your files, for your own use: do not share the installed folder.

After the install, Fastball.exe starts the game straight away. Hold Shift while starting it (or start it with
--menu) for its window: Play, Repair, Import Ryujinx save, and "Download and update" when GitHub has a new version
(it checks once a day; the update replaces this folder's files, and the game files are not needed again).
Settings: F1 in the game, or flags: Fastball.exe --fullscreen --fps-limit 120 (MLBTS\\mlb.exe --help lists them
all, from a console window).

Source, issues and help: https://github.com/sam110411/fastball
"""


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def run(cmd, **kw):
    print('+', ' '.join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def fetch(name):
    """The pinned download, from the cache or the network, checked against its sha256."""
    url, want = PINS[name]
    DL.mkdir(parents=True, exist_ok=True)
    path = DL / url.rsplit('/', 1)[1]
    if path.is_file() and sha256_file(path) == want:
        return path
    seed = ROOT / 'work' / 'toolchain' / 'dl' / path.name   # an earlier manual download
    if seed.is_file() and sha256_file(seed) == want:
        shutil.copy2(seed, path)
        return path
    print(f'download {url}', flush=True)
    part = path.with_name(path.name + '.part')
    with urllib.request.urlopen(url, timeout=120) as r, open(part, 'wb') as f:
        shutil.copyfileobj(r, f)
    got = sha256_file(part)
    if got != want:
        part.unlink()
        sys.exit(f'{url}: sha256 {got}, pinned {want}')
    part.replace(path)
    return path


def unpack(name):
    """A pinned zip extracted into release-work/<name> (once)."""
    dest = WORK / name
    if (dest / '.unpacked').is_file():
        return dest
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    with zipfile.ZipFile(fetch(name)) as z:
        z.extractall(dest)
    (dest / '.unpacked').write_text('')
    return dest


def copy_tree(src, dst, skip=lambda rel: False):
    for p in sorted(src.rglob('*')):
        rel = p.relative_to(src)
        if p.is_dir() or skip(rel.as_posix()):
            continue
        (dst / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dst / rel)


def clean_env(*path_dirs):
    """The Windows system directories and path_dirs only on PATH: what a player's PC has."""
    system = os.environ.get('SystemRoot', r'C:\Windows')
    keep = ('SYSTEMROOT', 'SYSTEMDRIVE', 'TEMP', 'TMP', 'USERPROFILE', 'LOCALAPPDATA', 'APPDATA', 'COMSPEC', 'PATHEXT',
            'NUMBER_OF_PROCESSORS', 'HOMEDRIVE', 'HOMEPATH')
    env = {k: v for k, v in os.environ.items() if k.upper() in keep}
    env['PATH'] = os.pathsep.join([*(str(Path(d)) for d in path_dirs), rf'{system}\System32', system])
    return env


# ---- python/ --------------------------------------------------------------------------------------------------------

def tcltk_files():
    """The python.org installer's tcltk payload, unpacked (not installed) into release-work/tcltk. After TotK Recomp."""
    dest = WORK / 'tcltk'
    if (dest / 'DLLs' / '_tkinter.pyd').is_file():
        return dest
    data = fetch('python-installer').read_bytes()
    cabs = []
    pos = data.find(b'MSCF\0\0\0\0')
    while pos >= 0:
        cabs.append((struct.unpack_from('<I', data, pos + 8)[0], pos))
        pos = data.find(b'MSCF\0\0\0\0', pos + 1)
    size, pos = max(cabs)   # the attached container holds all component MSIs
    src = WORK / 'tcltk-src'
    shutil.rmtree(src, ignore_errors=True)
    src.mkdir(parents=True)
    (src / 'container.cab').write_bytes(data[pos:pos + size])
    name, want = TCLTK_PAYLOAD
    expand = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'expand.exe'
    run([expand, '-F:' + name, src / 'container.cab', src], stdout=subprocess.DEVNULL)
    if sha256_file(src / name) != want:
        sys.exit(f'{src / name}: not the tcltk component of python {PY_VERSION}')
    msi = src / (name + '.msi')
    (src / name).rename(msi)
    shutil.rmtree(dest, ignore_errors=True)
    # Administrative image: copies the files, installs nothing. One command string: msiexec wants the quotes after the
    # '=' (TARGETDIR="C:\a b"), which a list argument does not give a path with spaces (exit 1639).
    cmd = f'msiexec /a "{msi}" /qn TARGETDIR="{dest}"'
    print('+', cmd, flush=True)
    subprocess.run(cmd, check=True)
    if not (dest / 'DLLs' / '_tkinter.pyd').is_file():
        sys.exit(f'{dest}: no _tkinter.pyd after msiexec /a')
    return dest


def build_python(out):
    py = out / 'python'
    with zipfile.ZipFile(fetch('python-embed')) as z:
        z.extractall(py)
    # The embeddable Python ignores the script directory and PYTHONPATH once a ._pth file exists, so the folder's own code
    # (installer/, and tools/*.py and recomp/*.py importing their siblings) has to be on the list.
    (py / 'python314._pth').write_text('python314.zip\n.\nLib/site-packages\n..\n../tools\n../recomp\n', encoding='utf-8')
    run([sys.executable, '-m', 'pip', 'install', '--quiet', '--disable-pip-version-check', '--target',
         py / 'Lib' / 'site-packages', '--platform', 'win_amd64', '--python-version', '3.14', '--implementation', 'cp',
         '--only-binary', ':all:', '-r', SRC / 'tools' / 'requirements-release.txt'])
    shutil.rmtree(py / 'Lib' / 'site-packages' / 'bin', ignore_errors=True)   # numpy's console scripts
    tk = tcltk_files()
    for dll in ('_tkinter.pyd', 'tcl90.dll', 'tcl9tk90.dll', 'zlib1.dll'):
        shutil.copy2(tk / 'DLLs' / dll, py / dll)
    copy_tree(tk / 'Lib' / 'tkinter', py / 'tkinter', lambda r: r.startswith('__pycache__'))
    copy_tree(tk / 'tcl', py / 'tcl', lambda r: r.endswith(('.lib', '.sh')) or r.startswith('nmake'))


# ---- llvm/ ----------------------------------------------------------------------------------------------------------

def build_llvm(out):
    src = unpack('llvm-mingw') / f'llvm-mingw-{LLVM_VERSION}-ucrt-x86_64'
    dst = out / 'llvm'

    def skip(rel):
        top = rel.split('/')[0]
        if top in LLVM_SKIP_DIRS:
            return True
        if top == 'bin':
            return rel not in {f'bin/{n}' for n in LLVM_BIN}
        if top == 'lib':   # lib/clang/<major>/include and the x86_64 Windows builtins; no sanitizers, no other targets
            if rel.startswith('lib/clang/') and '/include/' in rel:
                return False
            return not rel.endswith('/lib/windows/libclang_rt.builtins-x86_64.a')
        return Path(rel).suffix.lower() in {'.idl', '.tlb'} or rel.startswith('include/ddk/')

    copy_tree(src, dst, skip)
    return dst


# ---- runtime/, bin/ -------------------------------------------------------------------------------------------------

def build_runtime(out, llvm):
    deps = ROOT / 'work' / 'deps'
    gen = ROOT / 'work' / 'gen'
    if not (gen / 'layout.h').is_file():
        sys.exit('work/gen/layout.h is missing: run recomp/gen.py once (docs/runtime-port.md)')
    # inside SRC (build.py wants its --out below its root); a fresh library each time: SRC is a new export
    # one per CPU level: x86-64-v3 (AVX2) and x86-64-v2 for CPUs without it; the setup links the one the CPU runs
    # (installer/detect.py cpu_level). The v2 build checks its objects for instructions above v2 (tools/check_isa.py).
    rt = out / 'runtime'
    rt.mkdir(parents=True, exist_ok=True)
    for cpu, name in (('v3', 'libmlbrt.a'), ('v2', 'libmlbrt-v2.a')):
        lib_out = SRC / 'work' / f'release-rtlib-{cpu}'
        run([sys.executable, '-I', SRC / 'tools' / 'build.py', '--toolchain', llvm, '--make-runtime-lib', '--cpu', cpu,
             '--gen', gen, '--deps', deps, '--out', lib_out, '--ninja', unpack('ninja') / 'ninja.exe'], cwd=SRC)
        shutil.copy2(lib_out / name, rt / name)
    for p in sorted((SRC / 'runtime').rglob('*')):   # the headers modules.c and the generated C include
        if p.is_file() and p.suffix in ('.h', '.inc'):
            dst = rt / p.relative_to(SRC / 'runtime')
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dst)
    d = rt / 'deps'
    copy_tree(deps / 'include', d / 'include')
    (d / 'lib').mkdir(parents=True, exist_ok=True)
    shutil.copy2(deps / 'lib' / 'vulkan-1.lib', d / 'lib' / 'vulkan-1.lib')
    (d / 'bin').mkdir(parents=True, exist_ok=True)
    shutil.copy2(deps / 'bin' / 'SDL3.dll', d / 'bin' / 'SDL3.dll')


def build_bin(out):
    b = out / 'bin'
    b.mkdir(parents=True, exist_ok=True)
    shutil.copy2(unpack('ninja') / 'ninja.exe', b / 'ninja.exe')
    xl = ROOT / 'work' / 'xlate'
    have = (xl / 'totk_xlate.id').read_text(encoding='ascii').strip() if (xl / 'totk_xlate.id').is_file() else 'none'
    if have != build_xlate.identity():
        sys.exit('work/xlate/totk_xlate.exe is older than its sources: run tools/build_xlate.py')
    for f in ('totk_xlate.exe', 'totk_xlate.id'):
        shutil.copy2(xl / f, b / f)


def build_sources(out, version):
    copy_tree(SRC / 'recomp', out / 'recomp', lambda r: not r.endswith(('.py', '.txt')) or '__pycache__' in r)
    for f in TOOLS_FILES:
        (out / 'tools' / f).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(SRC / 'tools' / f, out / 'tools' / f)
    copy_tree(SRC / 'tools' / 'equipment', out / 'tools' / 'equipment',   # the setup's optional equipment step
              lambda r: '__pycache__' in r or r.startswith('tests/'))
    copy_tree(SRC / 'tools' / 'packages', out / 'tools' / 'packages',     # the setup's optional packages step
              lambda r: '__pycache__' in r or r.startswith('tests/'))
    copy_tree(SRC / 'installer', out / 'installer', lambda r: '__pycache__' in r or r.startswith('launcher/'))
    shutil.copy2(SRC / 'runtime' / 'mlb' / 'mlb.png', out / 'installer' / 'icon.png')
    lic = out / 'licenses'
    lic.mkdir(exist_ok=True)
    llvm_src = unpack('llvm-mingw') / f'llvm-mingw-{LLVM_VERSION}-ucrt-x86_64'
    for src, name in ((llvm_src / 'LICENSE.TXT', 'llvm-mingw-LICENSE.txt'),
                      (ROOT / 'work' / 'deps' / 'licenses' / 'SDL3.txt', 'SDL3-LICENSE.txt'),
                      (ROOT / 'work' / 'deps' / 'licenses' / 'Vulkan-Headers.txt', 'Vulkan-Headers-LICENSE.txt'),
                      (SRC / 'runtime' / 'core' / 'LICENSE.totk-recomp', 'totk-recomp-LICENSE.txt'),
                      (SRC / 'runtime' / 'third_party' / 'imgui' / 'LICENSE.txt', 'imgui-LICENSE.txt')):
        shutil.copy2(src, lic / name)
    for name in ('LICENSE', 'THIRD_PARTY_NOTICES.md', 'recomplauncher.json'):   # recomplauncher.json: events.ProgressLines
        shutil.copy2(SRC / name, out / name)
    (out / 'README.txt').write_text(README.format(version=version), encoding='utf-8')
    (out / 'VERSION').write_text(version + '\n', encoding='utf-8')


def build_launcher(out, llvm):
    tmp = WORK / 'launcher'
    tmp.mkdir(parents=True, exist_ok=True)
    run([llvm / 'bin' / 'llvm-windres.exe', '-I', SRC / 'runtime' / 'mlb', '-O', 'coff',
         SRC / 'installer' / 'launcher' / 'launcher.rc', '-o', tmp / 'launcher_rc.o'], env=clean_env(llvm / 'bin'))
    run([llvm / 'bin' / 'clang.exe', '-O2', '-mwindows',
         SRC / 'installer' / 'launcher' / 'launcher.c', tmp / 'launcher_rc.o', '-o', out / 'Fastball.exe'],
        env=clean_env(llvm / 'bin'))


# ---- checks ---------------------------------------------------------------------------------------------------------

def check_folder(out):
    """No game data, generated code, keys or game build in the folder; executables only where listed."""
    bad = []
    for p in sorted(out.rglob('*')):
        if not p.is_file():
            continue
        rel = p.relative_to(out).as_posix()
        data = p.read_bytes()
        for problem in check_content.problems(rel, data):
            if not problem.startswith('larger than'):
                bad.append(f'{rel}: {problem}')
        if p.suffix.lower() in ('.exe', '.dll', '.pyd', '.a', '.lib') and not rel.startswith(EXE_ALLOWED):
            bad.append(f'{rel}: an executable or library outside the allowed list')
        if p.name.lower() in ('mlb.exe', 'prod.keys', 'title.keys') or p.suffix.lower() in ('.nsp', '.xci', '.nca',
                                                                                             '.nso', '.nsz', '.xcz'):
            bad.append(f'{rel}: must never be in a release')
        # C sources and objects: only the toolchain's and Python's own (crt1.o, numpy's sources); ours would be the
        # generated code or its objects
        if p.suffix.lower() in ('.o', '.obj', '.c') and not rel.startswith(('llvm/', 'python/')):
            bad.append(f'{rel}: a C source or object outside the toolchain')
    if bad:
        sys.exit('make_release: the folder fails the content check:\n  ' + '\n  '.join(bad[:40]))
    print('content check: clean')


def smoke(out):
    py = out / 'python' / 'python.exe'
    env = clean_env(out / 'python')
    r = subprocess.run([str(py), '-I', '-c', 'import tkinter, numpy, lz4.block, installer.steps, nxextract, ncastorage; '
                        'print("python ok: Tcl", tkinter.Tcl().eval("info patchlevel"), "numpy", numpy.__version__)'],
                       cwd=out, env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit('bundled python check failed:\n' + r.stderr)
    print(r.stdout.strip())
    for script in SETUP_SCRIPTS:
        r = subprocess.run([str(py), '-I', '-X', 'utf8', script, '--help'], cwd=out, env=env, capture_output=True,
                           text=True)
        if r.returncode:
            sys.exit(f'{script} does not start with the bundled python:\n{r.stderr[-2000:]}')
    print(f'setup scripts start: {", ".join(SETUP_SCRIPTS)}')
    r = subprocess.run([str(py), '-I', '-m', 'installer.gui', '--selftest'], cwd=out, env=env, capture_output=True,
                       text=True, timeout=60)
    if r.returncode:
        sys.exit('window selftest failed:\n' + r.stderr)
    print('window selftest: ok')
    r = subprocess.run([str(py), '-I', '-m', 'installer.update', '--help'], cwd=out, env=env, capture_output=True,
                       text=True, timeout=60)
    if r.returncode:
        sys.exit('updater does not start with the bundled python:\n' + r.stderr[-2000:])
    # a setup run without game files: the folder check passes, then it must ask for the game files
    dest = WORK / 'smoke-dest'
    shutil.rmtree(dest, ignore_errors=True)
    r = subprocess.run([str(py), '-I', '-m', 'installer', '--dest', str(dest)], cwd=out, env=env, capture_output=True,
                       text=True, timeout=120)
    if r.returncode != 1 or 'Pick the base game file' not in r.stdout:
        sys.exit(f'setup smoke run: expected it to ask for the game files, got exit {r.returncode}:\n{r.stdout[-2000:]}')
    # the check step's CPU probe (installer/detect.py cpu_level) must compile with llvm/ and the runtime headers here:
    # its fallback would hide a broken probe and build v3 for some CPUs without the whole v3 set
    log = ''.join(p.read_text(encoding='utf-8', errors='replace') for p in (out / 'logs').glob('setup-*.log'))
    if 'cpu: x86-64-' not in log or '(CPUID)' not in log:
        sys.exit(f'setup smoke run: the CPU probe did not run (setup log):\n{log[-2000:]}')
    print('setup smoke run: ok (asks for the game files, CPU probe ran)')
    shutil.rmtree(out / 'logs', ignore_errors=True)
    shutil.rmtree(out / 'temp', ignore_errors=True)   # the probe's source and exe
    shutil.rmtree(dest, ignore_errors=True)
    smoke_packages(out)


def smoke_packages(out):
    """The packages step copies the server and only a few files of python/ into an install (installer/steps.py
    Packages.PYTHON), and the game runs the server on that copy. The same copy here must import the whole server and
    make a TLS server context, so a module the server starts to need that the list lacks stops the release, not a
    player's game."""
    sys.path.insert(0, str(out))
    try:
        from installer.steps import Packages
    finally:
        sys.path.pop(0)
    folder = WORK / 'smoke-packages'
    shutil.rmtree(folder, ignore_errors=True)
    py = folder / 'python'
    py.mkdir(parents=True)
    for f in (out / 'tools' / 'packages').glob('*.py'):
        shutil.copy2(f, folder / f.name)
    for pattern in Packages.PYTHON:
        found = sorted((out / 'python').glob(pattern))
        if not found:
            sys.exit(f'packages smoke: python/{pattern} is not in the release')
        for f in found:
            shutil.copy2(f, py / f.name)
    library = next(py.glob('python3??.zip'))
    (py / (library.stem + '._pth')).write_text(library.name + '\n.\n', encoding='ascii')
    code = ('import sys; sys.path.insert(0, sys.argv[1]); import ssl, server, handlers, cert, rpc; '
            'ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); print("packages server imports on its python copy")')
    r = subprocess.run([str(py / 'python.exe'), '-I', '-X', 'utf8', '-c', code, str(folder)], cwd=folder,
                       env=clean_env(), capture_output=True, text=True, timeout=60)
    if r.returncode:
        sys.exit('packages smoke: the server does not start on the copied python:\n' + r.stderr[-2000:])
    print(r.stdout.strip())
    shutil.rmtree(folder, ignore_errors=True)


def export_head():
    """A clean copy of the committed tree (git archive HEAD) in work/release-src: the release is made of exactly what is
    committed, whatever the working tree holds (work in progress stays out, and is never touched)."""
    dest = ROOT / 'work' / 'release-src'
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    tar = subprocess.run(['git', '-C', str(ROOT), 'archive', '--format=tar', 'HEAD'], check=True,
                         capture_output=True).stdout
    import io
    import tarfile
    with tarfile.open(fileobj=io.BytesIO(tar)) as t:
        t.extractall(dest, filter='data')
    return dest


def version(worktree):
    r = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True)
    if r.returncode or not r.stdout.strip():
        sys.exit('no git version: not a git checkout')
    dirty = subprocess.run(['git', '-C', str(ROOT), 'status', '--porcelain', '--untracked-files=no'],
                           capture_output=True, text=True).stdout.strip()
    if dirty and not worktree:
        print('make_release: the working tree has uncommitted changes; they are not in the release (it is built from '
              'HEAD)', file=sys.stderr)
    return ((SRC / 'VERSION').read_text(encoding='utf-8').strip() + '+' + r.stdout.strip()
            + ('.worktree' if worktree else ''))


def main():
    global SRC
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path, default=ROOT / 'work' / 'release')
    ap.add_argument('--no-zip', action='store_true')
    ap.add_argument('--from-worktree', action='store_true',
                    help='build from the working tree, uncommitted changes included (testing only, never published)')
    a = ap.parse_args()
    SRC = ROOT if a.from_worktree else export_head()
    ver = version(a.from_worktree)
    semver = ver.split('+')[0]
    folder = a.out / f'Fastball-{semver}'
    shutil.rmtree(folder, ignore_errors=True)
    folder.mkdir(parents=True)
    build_python(folder)
    llvm = build_llvm(folder)
    build_bin(folder)
    build_runtime(folder, llvm)
    build_sources(folder, ver)
    build_launcher(folder, llvm)
    check_folder(folder)
    smoke(folder)
    size = sum(p.stat().st_size for p in folder.rglob('*') if p.is_file())
    print(f'{folder}: {size / (1 << 20):.0f} MiB, version {ver}')
    if not a.no_zip:
        # the release, and the same for the updaters of setups up to 0.1.5 (the module docstring)
        for zname, extra in ((f'Fastball-{semver}-windows.zip', None),
                             (f'MLBTS-Setup-{semver}-windows.zip', 'MLBTS-Setup.exe')):
            zpath = a.out / zname
            zpath.unlink(missing_ok=True)
            with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
                for p in sorted(folder.rglob('*')):
                    if p.is_file():
                        z.write(p, Path(folder.name) / p.relative_to(folder))
                if extra:
                    z.write(folder / 'Fastball.exe', Path(folder.name) / extra)
            print(f'{zpath}: {zpath.stat().st_size / (1 << 20):.0f} MiB, sha256 {sha256_file(zpath)}')


if __name__ == '__main__':
    main()
