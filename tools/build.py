"""Build mlb.exe: the generated C (recomp/gen.py output) plus the host runtime, with LLVM clang + lld through Ninja.

    python -I tools/build.py [--gen work/gen] [--out work/build] [--clang PATH] [--jobs N] [--exefs DIR] [--version auto|1.0.20]
    python -I tools/build.py --toolchain <llvm-mingw> --out work/build-mingw          # the release's toolchain
    python -I tools/build.py --toolchain <llvm-mingw> --make-runtime-lib --out DIR    # DIR/libmlbrt.a, no game code
    python -I tools/build.py --toolchain <llvm-mingw> --runtime-lib libmlbrt.a --portable --gen G --out O   # an install

Inputs: --gen holds layout.json/layout.h/patches.h and one directory per module (main, sdk) from recomp/gen.py.
Writes --out/modules.c (layout table, linked modules, expected ExeFS hashes from tools/data/expected-<version>.json,
default paths, version), --out/build.ninja, objects under --out/obj, and --out/mlb.exe. Everything under --out is
derived from game code and stays out of git (work/ is ignored), except libmlbrt.a of --make-runtime-lib: the runtime
alone, which the release ships (docs/release.md). It reads only the shared headers of --gen (layout.h, patches.h) and
the button icons of tools/make_prompts.py.

Game versions: each supported one has tools/data/expected-<version>.json (1.0.15, 1.0.20). --version auto (the default)
takes the one whose main has the SHA-256 of --exefs/main (default: the dev ExeFS, work/update/exefs) and checks that
--gen was generated from that main (its image size), so an exe never pairs one version's code with another's checks.

The default target is MSVC (LLVM clang in Program Files, Visual Studio's libraries), the dev build. --toolchain builds
for x86_64-w64-mingw32 with llvm-mingw, whose libraries may be redistributed: the release ships it, and the setup
compiles the player's game code with it.

On Linux (docs/linux.md) it builds work/build/mlb, an ELF executable, with clang, lld and ninja from PATH (or --clang):
the runtime's *_sdl.* files and platform/linux.c take the place of the Win32 ones, Vulkan and SDL3 are the system's
(libvulkan.so.1, libSDL3.so.0; a libSDL3.so.0 beside mlb comes first), libstdc++ is linked statically. The Vulkan, VMA
and SDL3 headers still come from --deps (tools/fetch_deps.py), which are the same on every OS.
"""
import argparse
import fnmatch
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from check_content import GENERATED_MARKER  # noqa: E402

LINUX = sys.platform.startswith('linux')
DEFAULT_CLANG = (shutil.which('clang') or 'clang') if LINUX else r'C:\Program Files\LLVM\bin\clang.exe'
# Generated code: TotK Recomp tools/build_game.py GEN_FLAGS @ ae30a61 (LAS Recomp's flags plus -march).
# fastball: -O2 instead of TotK's -O1. In a match at an unlocked frame rate the game's own code (main thread and
# its three job workers) holds the frame, not the GPU; -O2 gave 3-6% more fps in every game state measured on
# 2026-11-17 (exhibition, MLB_FPS_LIMIT=240, Ryzen 5 5600X: 109.5 -> 113.8 fps over the match states) for about 13%
# more compile time per file.
GEN_FLAGS = ['-O2', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label', '-fvisibility=hidden',
             '-march=x86-64-v3']
RT_FLAGS = ['-O2', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing', '-march=x86-64-v3', '-std=gnu11',
            '-D_CRT_SECURE_NO_WARNINGS']
LIBS = ['bcrypt', 'ws2_32', 'user32', 'kernel32', 'advapi32', 'ntdll', 'synchronization', 'winmm', 'psapi', 'shell32', 'ole32',
        'dbghelp', 'gdi32', 'dwmapi', 'imm32', 'uuid']
RUNTIME_SOURCES = ['runtime/core/*.c', 'runtime/core/platform/*.c',
                   'runtime/mlb/*.c', 'runtime/gfx/*.c', 'runtime/gfx/*.cpp',
                   # the in-game settings menu (F1) and Dear ImGui, which draws it
                   'runtime/gfx/ui/*.cpp', 'runtime/third_party/imgui/*.cpp',
                   'runtime/third_party/imgui/backends/*.cpp']
DEPS = ROOT / 'work' / 'deps'   # tools/fetch_deps.py: Vulkan headers, VMA, vulkan-1.lib
CXX_FLAGS = ['-O2', '-g', '-std=c++17', '-march=x86-64-v3', '-D_CRT_SECURE_NO_WARNINGS', '-Wno-nullability-completeness']
LINUX_LIBS = ['vulkan', 'SDL3', 'm', 'pthread', 'dl']
# One OS's files of a pair: the Win32 window, menu, software keyboard, platform, audio and sampler, or the SDL3 and
# Linux ones in their place (runtime/core/platform/platform.h, runtime/gfx/gfx_wsys.h).
WIN_ONLY = ['*_win.c', '*_win.cpp', 'win32.c', 'audio_win32.c']
LINUX_ONLY = ['*_sdl.c', '*_sdl.cpp', 'linux.c']


def for_this_os(p):
    other = WIN_ONLY if LINUX else LINUX_ONLY
    return not any(fnmatch.fnmatch(p.name, pat) for pat in other)


def c_string(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def game_version(want, exefs, layout):
    """The expected-<version>.json for --version (auto: the one matching exefs/main), checked against --gen's main."""
    data = ROOT / 'tools' / 'data'
    if want != 'auto':
        e = json.loads((data / f'expected-{want}.json').read_text())
    else:
        main = exefs / 'main'
        if not main.is_file():
            raise SystemExit(f'{main} is missing: pass --exefs (the ExeFS recomp/gen.py read) or --version')
        h = hashlib.sha256(main.read_bytes()).hexdigest()
        e = next((j for p in sorted(data.glob('expected-*.json')) for j in [json.loads(p.read_text())]
                  if j['exefs_sha256']['main'] == h), None)
        if e is None:
            known = ', '.join(p.stem[9:] for p in sorted(data.glob('expected-*.json')))
            raise SystemExit(f'{main} is not the code of a supported game version ({known})')
    size = next((int(m['size'], 16) for m in layout if m['name'] == 'main'), None)
    if size is not None and size != int(e['main_image_size'], 16):
        raise SystemExit(f'--gen holds main of another game version ({size:#x} bytes, {e["display_version"]} has '
                         f'{e["main_image_size"]}): run recomp/gen.py on the {e["display_version"]} ExeFS again')
    return e


def module_glue(layout, linked, hashes, paths, version, game):
    """modules.c: the layout table, the linked modules' function tables (dispatch.h), expected ExeFS hashes, the
    default game paths and the version strings (host.h). After TotK Recomp tools/build_game.py module_glue."""
    out = [f'// Module glue written by tools/build.py; do not edit. {GENERATED_MARKER}',
           '#include "dispatch.h"', '#include "host.h"', '#include "layout.h"', '']
    for m in layout:
        u = m['name'].upper()
        out.append(f'_Static_assert(MOD_{u}_BASE == {m["base"]}ull && MOD_{u}_SIZE == {m["size"]}ull, '
                   f'"layout.h and layout.json disagree on {m["name"]}");')
    out += ['', 'const RtLayoutEntry rt_layout[] = {']
    out += [f'    {{"{m["name"]}", MOD_{m["name"].upper()}_BASE, MOD_{m["name"].upper()}_SIZE}},' for m in layout]
    out += ['};', f'const int rt_layout_count = {len(layout)};', '']
    for name in linked:
        out += [f'extern const uint64_t {name}_fn_addrs[];', f'extern const GuestFn {name}_fn_ptrs[];',
                f'extern const uint64_t {name}_fn_count;']
    out += ['', 'int rt_linked_modules(RtModule *out, int max) {', '    int n = 0;']
    for name in linked:
        u = name.upper()
        out.append(f'    if (n < max) out[n++] = (RtModule){{"{name}", MOD_{u}_BASE, MOD_{u}_SIZE, {name}_fn_addrs, '
                   f'{name}_fn_ptrs, {name}_fn_count}};')
    out += ['    return n;', '}', '', 'const RtFileHash rt_exefs_sha256[] = {']
    out += [f'    {{{c_string(f)}, {c_string(h)}}},' for f, h in sorted(hashes.items())]
    out += ['};', f'const int rt_exefs_sha256_count = {len(hashes)};', '']
    for name in ('exefs', 'romfs', 'profile'):
        out.append(f'const char rt_default_{name}[] = {c_string(paths[name])};')
    out += ['// 0: not a TotK build. Only TotK self-test probes (loader.c, ipc.c) read it.',
            'const int rt_game_version = 0;', f'const char rt_port_version[] = {c_string(version)};',
            f'const char rt_game_build[] = {c_string(game)};', '']
    return '\n'.join(out)


def compiler_rt(clang):
    """clang_rt.builtins for the MSVC target (128-bit division, __builtin_cpu_supports), which clang does not link by
    itself there."""
    libs = sorted(Path(clang).resolve().parent.parent.glob('lib/clang/*/lib/windows/clang_rt.builtins-x86_64.lib'))
    if not libs:
        raise SystemExit(f'clang_rt.builtins-x86_64.lib not found next to {clang}')
    return libs[-1].as_posix()


def rel(p):
    """Path relative to the repository root (ninja runs there), so no flag carries the profile path's spaces."""
    p = Path(p).resolve()
    return Path(os.path.relpath(p, ROOT)).as_posix()


def ninja_path(p):
    return rel(p).replace(':', '$:').replace(' ', '$ ')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--gen', type=Path, default=ROOT / 'work' / 'gen')
    ap.add_argument('--out', type=Path, default=ROOT / 'work' / 'build')
    ap.add_argument('--clang', default=os.environ.get('CLANG', DEFAULT_CLANG))
    ap.add_argument('--jobs', type=int, default=0)
    ap.add_argument('--version', default='auto', help='the game version (tools/data/expected-<version>.json); auto: '
                                                       'the one whose main is --exefs/main')
    ap.add_argument('--exefs', type=Path, help='the ExeFS --gen was generated from, for --version auto (default: '
                                               'work/update/exefs)')
    ap.add_argument('--toolchain', type=Path, help='an llvm-mingw folder (bin/clang.exe): build for x86_64-w64-mingw32, '
                                                  'as the release does, instead of the MSVC target')
    ap.add_argument('--deps', type=Path, default=DEPS, help='Vulkan, VMA and SDL3 headers, lib/vulkan-1.lib and '
                                                           'bin/SDL3.dll (tools/fetch_deps.py)')
    ap.add_argument('--ninja', type=Path, help='ninja.exe (default: on PATH, else beside this Python)')
    ap.add_argument('--make-runtime-lib', action='store_true',
                    help='build only <out>/libmlbrt.a: the runtime objects and the icon/manifest resource, no game code '
                         '(the release ships it; docs/release.md)')
    ap.add_argument('--runtime-lib', type=Path, help='link this prebuilt libmlbrt.a instead of compiling the runtime')
    ap.add_argument('--portable', action='store_true', help="default paths beside mlb.exe (game/exefs, game/romfs, "
                                                           "profile) instead of the repository's work/ folders")
    a = ap.parse_args()
    mingw = a.toolchain is not None
    if mingw:
        a.clang = str(a.toolchain.resolve() / 'bin' / 'clang.exe')
    if (a.make_runtime_lib or a.runtime_lib) and not mingw:
        raise SystemExit('--make-runtime-lib and --runtime-lib need --toolchain (releases are built with llvm-mingw)')
    deps = a.deps.resolve()
    gen, out = a.gen.resolve(), a.out.resolve()
    layout = json.loads((gen / 'layout.json').read_text())
    linked = [m['name'] for m in layout if (gen / m['name'] / f'{m["name"]}_table.c').exists()]
    if not linked and not a.make_runtime_lib:
        raise SystemExit(f'no generated modules under {gen}: run recomp/gen.py first')
    # the runtime library holds no game code and no version (modules.c, written per install, has it)
    expected = (json.loads((ROOT / 'tools' / 'data' / 'expected-1.0.20.json').read_text()) if a.make_runtime_lib else
                game_version(a.version, (a.exefs or ROOT / 'work' / 'update' / 'exefs').resolve(), layout))
    port_version = (ROOT / 'VERSION').read_text().strip()
    if a.portable:   # main.c beside_exe takes them beside mlb.exe
        paths = {'exefs': 'game/exefs', 'romfs': 'game/romfs', 'profile': 'profile'}
    else:
        paths = {'exefs': (ROOT / 'work' / 'update' / 'exefs').as_posix(),
                 'romfs': (ROOT / 'work' / 'romfs' / 'romfs').as_posix(), 'profile': (ROOT / 'work' / 'profile').as_posix()}
    out.mkdir(parents=True, exist_ok=True)
    glue = out / 'modules.c'
    text = module_glue(layout, linked, expected['exefs_sha256'], paths, port_version, expected['display_version'])
    if not a.make_runtime_lib and (not glue.exists() or glue.read_text() != text):
        glue.write_text(text)

    if a.runtime_lib:   # modules.c is per install: the layout, the linked modules, the default paths, the version
        rt_srcs = [glue]
    else:
        rt_srcs = sorted({p for pat in RUNTIME_SOURCES for p in ROOT.glob(pat) if for_this_os(p)})
        if not a.make_runtime_lib:
            rt_srcs.append(glue)
    gen_srcs = [] if a.make_runtime_lib else sorted(p for name in linked for p in (gen / name).glob('*.c'))
    if not (deps / 'include' / 'vulkan' / 'vulkan.h').is_file() or (not LINUX and not (deps / 'lib' / 'vulkan-1.lib').is_file()):
        raise SystemExit(f'{deps} is missing the Vulkan headers and import library: run tools/fetch_deps.py')
    if not (deps / 'include' / 'SDL3' / 'SDL.h').is_file() or (not LINUX and not (deps / 'bin' / 'SDL3.dll').is_file()):
        raise SystemExit(f'{deps} is missing SDL3 (controllers): run tools/fetch_deps.py')
    # runtime/mlb/hid.c loads SDL3.dll from mlb.exe's folder at run time. A running game holds it: keep the old copy then.
    # On Linux mlb links the system's libSDL3.so.0.
    sdl_src, sdl_dst = deps / 'bin' / 'SDL3.dll', out / 'SDL3.dll'
    if not LINUX and not a.make_runtime_lib and (not sdl_dst.is_file() or sdl_dst.read_bytes() != sdl_src.read_bytes()):
        try:
            shutil.copyfile(sdl_src, sdl_dst)
        except OSError as e:
            print(f'warning: {sdl_dst} not updated ({e})')
    # runtime/mlb/prompts.c: the Xbox and PlayStation button icons, drawn and compressed by tools/make_prompts.py (about
    # half a minute), again only when that script changed. A prebuilt runtime library holds them already.
    icons, icon_tool = gen / 'prompt_icons.h', ROOT / 'tools' / 'make_prompts.py'
    if not a.runtime_lib and (not icons.is_file() or icons.stat().st_mtime < icon_tool.stat().st_mtime):
        if subprocess.run([sys.executable, '-I', str(icon_tool), '--out', str(icons)], cwd=ROOT).returncode:
            raise SystemExit('tools/make_prompts.py failed')
    inc_rt = ['-I', 'runtime/mlb', '-I', 'runtime/core', '-I', 'runtime/gfx', '-I', rel(gen), '-I', rel(deps / 'include'),
              '-I', rel(deps / 'include' / 'vma'), '-I', 'runtime/third_party/imgui']
    inc_gen = ['-I', 'runtime/core', '-I', rel(gen)]
    for d in (rel(gen), rel(out), rel(deps)):
        if ' ' in d:
            raise SystemExit(f'{d}: keep --gen, --out and --deps inside the repository (or the release folder), '
                             'without spaces')
    exe = out / ('mlb' if LINUX else 'mlb.exe')
    bin_dir = Path(a.clang).parent
    rt_flags, cxx_flags = list(RT_FLAGS), list(CXX_FLAGS)
    vulkan = rel(deps / 'lib' / 'vulkan-1.lib')
    if LINUX:
        # $ORIGIN: a libSDL3.so.0 next to mlb comes before the system's (a release's own copy). libstdc++ static: the
        # menu's C++ needs no particular one on the player's system.
        clangxx = Path(a.clang).with_name(Path(a.clang).name.replace('clang', 'clang++'))
        link = (f'"{clangxx.as_posix()}" -fuse-ld=lld -g @$out.rsp -o $out {" ".join("-l" + l for l in LINUX_LIBS)} '
                f"-static-libstdc++ -static-libgcc -Wl,-rpath,'$$ORIGIN' -Wl,-Map={rel(out)}/mlb.map")
        rc = 'true'
        ar = 'llvm-ar' if shutil.which('llvm-ar') else 'ar'
    elif mingw:
        # llvm-mingw: Windows 10 API level (its headers default lower); libc++, libunwind and winpthread linked
        # statically (-static), so mlb.exe needs only the system DLLs and the UCRT. A prebuilt runtime library is linked
        # whole: the game code alone references little of it (not main(), not the resources).
        rt_flags.append('-D_WIN32_WINNT=0x0A00')
        cxx_flags.append('-D_WIN32_WINNT=0x0A00')
        whole = f'-Wl,--whole-archive {rel(a.runtime_lib)} -Wl,--no-whole-archive ' if a.runtime_lib else ''
        # an installed game is a windowed program: no console window of log lines beside it (the log file stays;
        # plat_fatal_notice shows a fatal error in a message box instead)
        whole += '-mwindows ' if a.portable else ''
        link = (f'"{(bin_dir / "clang++.exe").as_posix()}" -fuse-ld=lld -g -static @$out.rsp {whole}-o $out '
                f'{" ".join("-l" + l for l in LIBS)} {vulkan} -Wl,--stack,8388608 -Wl,-Map={rel(out)}/mlb.map')
        rc = f'"{(bin_dir / "llvm-windres.exe").as_posix()}" -I runtime/mlb -O coff $in -o $out'
    else:
        link = (f'$cc -fuse-ld=lld -g @$out.rsp -o $out {" ".join("-l" + l for l in LIBS)} '
                f'"{compiler_rt(a.clang)}" {vulkan} '
                f'-Wl,/STACK:8388608 -Wl,/errorlimit:0 -Wl,/MAP:{rel(out)}/mlb.map')
        rc = f'"{(bin_dir / "llvm-rc.exe").as_posix()}" /nologo /I runtime/mlb /FO $out $in'
    if not LINUX:
        ar = f'"{(bin_dir / "llvm-ar.exe").as_posix()}"'
    lines = [f'# {GENERATED_MARKER}: written by tools/build.py', f'builddir = {rel(out)}', f'cc = "{Path(a.clang).as_posix()}"',
             f'genflags = {" ".join(GEN_FLAGS)} {" ".join(inc_gen)}',
             f'rtflags = {" ".join(rt_flags)} {" ".join(inc_rt)}',
             # fastball: the generated code tracks its headers too (runtime/core/simd.h is inlined everywhere: a
             # change there left every game object stale)
             'rule cc_gen', '  command = $cc $genflags -MD -MF $out.d -c $in -o $out', '  depfile = $out.d',
             '  deps = gcc', '  description = CC $in',
             'rule cc_rt', '  command = $cc $rtflags -MD -MF $out.d -c $in -o $out', '  depfile = $out.d',
             '  deps = gcc', '  description = CC $in',
             f'cxxflags = {" ".join(cxx_flags)} {" ".join(inc_rt)}',
             'rule cxx_rt', '  command = $cc -x c++ $cxxflags -MD -MF $out.d -c $in -o $out', '  depfile = $out.d',
             '  deps = gcc', '  description = CXX $in',
             'rule rc', f'  command = {rc}', '  description = RC $in',
             # a fresh archive each time, so a removed source leaves no stale member
             'rule ar', (f'  command = rm -f $out && {ar} rcs $out @$out.rsp' if LINUX else
                         f'  command = cmd /c del /q "$out" 2>nul & {ar} rcs $out @$out.rsp'), '  rspfile = $out.rsp',
             '  rspfile_content = $in', '  description = AR $out',
             'rule link', f'  command = {link}', '  rspfile = $out.rsp', '  rspfile_content = $in',
             '  description = LINK $out', '']
    objs = []
    for s in rt_srcs:
        sub = s.relative_to(ROOT) if s.is_relative_to(ROOT) else Path(s.name)
        o = out / "obj" / "rt" / sub.with_suffix(".o")
        lines.append(f'build {ninja_path(o)}: {"cxx_rt" if s.suffix == ".cpp" else "cc_rt"} {ninja_path(s)}')
        objs.append(o)
    if not a.runtime_lib and not LINUX:   # the icon and the manifest (runtime/mlb/mlb.rc); a prebuilt library holds them
        res = out / 'obj' / 'rt' / ('mlb.res.o' if mingw else 'mlb.res')
        lines.append(f'build {ninja_path(res)}: rc runtime/mlb/mlb.rc | runtime/mlb/mlb.manifest runtime/mlb/mlb.ico')
        objs.append(res)
    for s in gen_srcs:
        o = out / 'obj' / 'gen' / s.parent.name / (s.stem + '.o')
        lines.append(f'build {ninja_path(o)}: cc_gen {ninja_path(s)}')
        objs.append(o)
    if a.make_runtime_lib:
        target = out / 'libmlbrt.a'
        lines.append(f'build {ninja_path(target)}: ar {" ".join(ninja_path(o) for o in objs)}')
    else:
        target = exe
        implicit = f' | {ninja_path(a.runtime_lib)}' if a.runtime_lib else ''
        lines.append(f'build {ninja_path(exe)}: link {" ".join(ninja_path(o) for o in objs)}{implicit}')
    lines.append(f'default {ninja_path(target)}')
    nf = out / 'build.ninja'
    nf.write_text('\n'.join(lines) + '\n')
    ninja = str(a.ninja) if a.ninja else shutil.which('ninja') or str(Path(sys.executable).parent / 'ninja.exe')
    cmd = [ninja, '-f', rel(nf), '-k', '0'] + (['-j', str(a.jobs)] if a.jobs else [])   # every failure, not the first
    sys.exit(subprocess.run(cmd, cwd=ROOT).returncode)


if __name__ == '__main__':
    main()
