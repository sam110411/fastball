"""Build mlb.exe: the generated C (recomp/gen.py output) plus the host runtime, with LLVM clang + lld through Ninja.

    python -I tools/build.py [--gen work/gen] [--out work/build] [--clang PATH] [--jobs N] [--exefs DIR] [--version auto|1.0.20]
    python -I tools/build.py --toolchain <llvm-mingw> --out work/build-mingw          # the release's toolchain
    python -I tools/build.py --toolchain <llvm-mingw> --make-runtime-lib --out DIR    # DIR/libmlbrt.a, no game code
    python -I tools/build.py --toolchain <llvm-mingw> --runtime-lib libmlbrt.a --portable --gen G --out O   # an install
    python -I tools/build.py --cpu v2 --out work/build-v2                             # for CPUs without AVX2

--cpu picks the instruction set of the runtime and the game code: v3 (x86-64-v3: AVX2, FMA, BMI2; the default) or v2
(x86-64-v2: SSE4.2, POPCNT) for CPUs without AVX2, which runs slower (no AVX, and runtime/core/fma.c computes the
Switch's fused multiply-adds in software). --make-runtime-lib --cpu v2 writes libmlbrt-v2.a. Inline assembly, target
attributes and intrinsics are not bound by -march, so a v2 build checks the runtime's objects (tools/check_isa.py).
mlb.exe checks the CPU before main (runtime/mlb/cpucheck.c), and refuses game code and a runtime of different levels.

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
"""
import argparse
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

DEFAULT_CLANG = r'C:\Program Files\LLVM\bin\clang.exe'
# Generated code: TotK Recomp tools/build_game.py GEN_FLAGS @ ae30a61 (LAS Recomp's flags plus -march).
# fastball: -O2 instead of TotK's -O1. In a match at an unlocked frame rate the game's own code (main thread and
# its three job workers) holds the frame, not the GPU; -O2 gave 3-6% more fps in every game state measured on
# 2026-11-17 (exhibition, MLB_FPS_LIMIT=240, Ryzen 5 5600X: 109.5 -> 113.8 fps over the match states) for about 13%
# more compile time per file.
GEN_FLAGS = ['-O2', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label', '-fvisibility=hidden',
             '-march=x86-64-v3']
# fastball: --cpu v2 puts -march=x86-64-v2 in place of each -march=x86-64-v3 (TotK Recomp tools/build_game.py set_cpu).
RT_FLAGS = ['-O2', '-g', '-Wall', '-Wno-unused-function', '-fno-strict-aliasing', '-march=x86-64-v3', '-std=gnu11',
            '-D_CRT_SECURE_NO_WARNINGS']
LIBS = ['bcrypt', 'ws2_32', 'user32', 'kernel32', 'advapi32', 'ntdll', 'synchronization', 'winmm', 'psapi', 'shell32', 'ole32',
        'dbghelp', 'gdi32', 'dwmapi', 'imm32', 'uuid']
RUNTIME_SOURCES = ['runtime/core/*.c', 'runtime/core/platform/common.c', 'runtime/core/platform/win32.c',
                   'runtime/core/platform/audio_win32.c', 'runtime/core/platform/prof_win.c',
                   'runtime/mlb/*.c', 'runtime/gfx/*.c', 'runtime/gfx/*.cpp',
                   # the in-game settings menu (F1) and Dear ImGui, which draws it
                   'runtime/gfx/ui/*.cpp', 'runtime/third_party/imgui/*.cpp',
                   'runtime/third_party/imgui/backends/*.cpp']
DEPS = ROOT / 'work' / 'deps'   # tools/fetch_deps.py: Vulkan headers, VMA, vulkan-1.lib
CXX_FLAGS = ['-O2', '-g', '-std=c++17', '-march=x86-64-v3', '-D_CRT_SECURE_NO_WARNINGS', '-Wno-nullability-completeness']
# fastball: profile-guided optimisation of the runtime alone (--pgo), after TotK Recomp tools/build_game.py set_pgo
# @ ae30a61. The replay thread is runtime code and most of a slow frame; the profile, made by an instrumented build
# playing the attract match (tools/pgo.py), names only runtime functions and holds their counts, so the release's
# libmlbrt.a is built with it and players need nothing. The game's code is never instrumented or built with a profile:
# its counts would be data taken from the game. A static function's profile name carries the base name of its file
# only, so a build from another folder (the release's export) finds the same names.
RUNTIME_PROFILE = ROOT / 'tools' / 'data' / 'runtime.profdata'
PGO_NAMES = ['-mllvm', '-static-func-full-module-prefix=false']
# Compiled for baseline x86-64 whatever --cpu says: the CPU check that runs before main (it must not stop on an AVX2
# instruction of its own on the CPUs it is there to catch)
BASELINE_SOURCES = {'runtime/mlb/cpucheck.c'}


def set_march(flags, march):
    return [march if f.startswith('-march=') else f for f in flags]


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


def module_glue(layout, linked, hashes, paths, version, game, cpu_level):
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
            f'const char rt_game_build[] = {c_string(game)};',
            '// the x86-64 level the generated code was compiled for (tools/build.py --cpu); runtime/mlb/cpucheck.c',
            f'const int rt_gen_cpu_level = {cpu_level};', '']
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
    ap.add_argument('--cpu', default='v3', choices=('v3', 'v2'),
                    help='instruction set: v3 (x86-64-v3, AVX2/FMA/BMI2, the default) or v2 (x86-64-v2, SSE4.2/POPCNT: '
                         'for CPUs without AVX2; slower)')
    ap.add_argument('--pgo', default='auto', metavar='auto|off|gen|FILE',
                    help='profile-guided optimisation of the runtime: auto (default) uses tools/data/runtime.profdata '
                         'when it exists, gen builds a runtime that counts (tools/pgo.py), FILE another .profdata')
    a = ap.parse_args()
    mingw = a.toolchain is not None
    if mingw:
        a.clang = str(a.toolchain.resolve() / 'bin' / 'clang.exe')
    if (a.make_runtime_lib or a.runtime_lib) and not mingw:
        raise SystemExit('--make-runtime-lib and --runtime-lib need --toolchain (releases are built with llvm-mingw)')
    cpu_level = 3 if a.cpu == 'v3' else 2
    lib_name = 'libmlbrt.a' if a.cpu == 'v3' else f'libmlbrt-{a.cpu}.a'
    if a.runtime_lib and a.runtime_lib.name.endswith('-v2.a') != (a.cpu == 'v2'):
        raise SystemExit(f'--runtime-lib {a.runtime_lib.name} is not the runtime for --cpu {a.cpu} ({lib_name})')
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
    text = module_glue(layout, linked, expected['exefs_sha256'], paths, port_version, expected['display_version'],
                       cpu_level)
    if not a.make_runtime_lib and (not glue.exists() or glue.read_text() != text):
        glue.write_text(text)

    if a.runtime_lib:   # modules.c is per install: the layout, the linked modules, the default paths, the version
        rt_srcs = [glue]
    else:
        rt_srcs = sorted({p for pat in RUNTIME_SOURCES for p in ROOT.glob(pat)})
        if not a.make_runtime_lib:
            rt_srcs.append(glue)
    gen_srcs = [] if a.make_runtime_lib else sorted(p for name in linked for p in (gen / name).glob('*.c'))
    if not (deps / 'lib' / 'vulkan-1.lib').is_file():
        raise SystemExit(f'{deps} is missing the Vulkan headers and import library: run tools/fetch_deps.py')
    if not (deps / 'include' / 'SDL3' / 'SDL.h').is_file() or not (deps / 'bin' / 'SDL3.dll').is_file():
        raise SystemExit(f'{deps} is missing SDL3 (controllers): run tools/fetch_deps.py')
    # runtime/mlb/hid.c loads SDL3.dll from mlb.exe's folder at run time. A running game holds it: keep the old copy then.
    sdl_src, sdl_dst = deps / 'bin' / 'SDL3.dll', out / 'SDL3.dll'
    if not a.make_runtime_lib and (not sdl_dst.is_file() or sdl_dst.read_bytes() != sdl_src.read_bytes()):
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
    exe = out / 'mlb.exe'
    bin_dir = Path(a.clang).parent
    march = f'-march=x86-64-{a.cpu}'
    gen_flags = set_march(GEN_FLAGS, march)
    rt_flags = set_march(RT_FLAGS, march) + [f'-DRT_CPU_LEVEL={cpu_level}']
    cxx_flags = set_march(CXX_FLAGS, march) + [f'-DRT_CPU_LEVEL={cpu_level}']
    pgo_link, pgo_dep = '', ''
    profile = None if a.pgo in ('off', 'gen') else RUNTIME_PROFILE if a.pgo == 'auto' else Path(a.pgo).resolve()
    if a.pgo == 'gen':
        # the counts go to <mlb.exe's folder>/pgo every minute and at exit (runtime/core/platform/win32.c)
        pgo_flags = ['-fprofile-generate', '-DTOTK_PGO_GEN']
        pgo_link = '-fprofile-generate '   # links clang_rt.profile
    elif profile and profile.is_file():
        # unprofiled: code the training never ran; out-of-date and backend-plugin: functions changed since the training,
        # which compile as without a profile
        pgo_flags = [f'-fprofile-use={rel(profile)}', '-Wno-profile-instr-unprofiled', '-Wno-profile-instr-out-of-date',
                     '-Wno-backend-plugin']
        pgo_dep = f' | {ninja_path(profile)}'   # a new profile rebuilds the runtime
    elif a.pgo not in ('auto', 'off'):
        raise SystemExit(f'--pgo {a.pgo}: no such profile')
    else:
        pgo_flags = []
    if pgo_flags:
        rt_flags += pgo_flags + PGO_NAMES
        cxx_flags += pgo_flags + PGO_NAMES
    vulkan = rel(deps / 'lib' / 'vulkan-1.lib')
    if mingw:
        # llvm-mingw: Windows 10 API level (its headers default lower); libc++, libunwind and winpthread linked
        # statically (-static), so mlb.exe needs only the system DLLs and the UCRT. A prebuilt runtime library is linked
        # whole: the game code alone references little of it (not main(), not the resources).
        rt_flags.append('-D_WIN32_WINNT=0x0A00')
        cxx_flags.append('-D_WIN32_WINNT=0x0A00')
        whole = f'-Wl,--whole-archive {rel(a.runtime_lib)} -Wl,--no-whole-archive ' if a.runtime_lib else ''
        # an installed game is a windowed program: no console window of log lines beside it (the log file stays;
        # plat_fatal_notice shows a fatal error in a message box instead)
        whole += '-mwindows ' if a.portable else ''
        link = (f'"{(bin_dir / "clang++.exe").as_posix()}" -fuse-ld=lld -g -static {pgo_link}@$out.rsp {whole}-o $out '
                f'{" ".join("-l" + l for l in LIBS)} {vulkan} -Wl,--stack,8388608 -Wl,-Map={rel(out)}/mlb.map')
        rc = f'"{(bin_dir / "llvm-windres.exe").as_posix()}" -I runtime/mlb -O coff $in -o $out'
    else:
        link = (f'$cc -fuse-ld=lld -g {pgo_link}@$out.rsp -o $out {" ".join("-l" + l for l in LIBS)} '
                f'"{compiler_rt(a.clang)}" {vulkan} '
                f'-Wl,/STACK:8388608 -Wl,/errorlimit:0 -Wl,/MAP:{rel(out)}/mlb.map')
        rc = f'"{(bin_dir / "llvm-rc.exe").as_posix()}" /nologo /I runtime/mlb /FO $out $in'
    ar = f'"{(bin_dir / "llvm-ar.exe").as_posix()}"'
    lines = [f'# {GENERATED_MARKER}: written by tools/build.py', f'builddir = {rel(out)}', f'cc = "{Path(a.clang).as_posix()}"',
             f'genflags = {" ".join(gen_flags)} {" ".join(inc_gen)}',
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
             'rule ar', f'  command = cmd /c del /q "$out" 2>nul & {ar} rcs $out @$out.rsp', '  rspfile = $out.rsp',
             '  rspfile_content = $in', '  description = AR $out',
             'rule link', f'  command = {link}', '  rspfile = $out.rsp', '  rspfile_content = $in',
             '  description = LINK $out', '']
    objs = []
    for s in rt_srcs:
        sub = s.relative_to(ROOT) if s.is_relative_to(ROOT) else Path(s.name)
        o = out / "obj" / "rt" / sub.with_suffix(".o")
        lines.append(f'build {ninja_path(o)}: {"cxx_rt" if s.suffix == ".cpp" else "cc_rt"} {ninja_path(s)}{pgo_dep}')
        if sub.as_posix() in BASELINE_SOURCES:
            lines.append(f'  rtflags = {" ".join(set_march(rt_flags, "-march=x86-64"))} {" ".join(inc_rt)}')
        objs.append(o)
    if not a.runtime_lib:   # the icon and the manifest (runtime/mlb/mlb.rc); a prebuilt library holds them
        res = out / 'obj' / 'rt' / ('mlb.res.o' if mingw else 'mlb.res')
        lines.append(f'build {ninja_path(res)}: rc runtime/mlb/mlb.rc | runtime/mlb/mlb.manifest runtime/mlb/mlb.ico')
        objs.append(res)
    for s in gen_srcs:
        o = out / 'obj' / 'gen' / s.parent.name / (s.stem + '.o')
        lines.append(f'build {ninja_path(o)}: cc_gen {ninja_path(s)}')
        objs.append(o)
    if a.make_runtime_lib:
        target = out / lib_name
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
    code = subprocess.run(cmd, cwd=ROOT).returncode
    if code == 0 and a.cpu == 'v2' and not a.runtime_lib:
        # Inline assembly, target attributes and intrinsics pass -march=x86-64-v2 by; one vmovdqa of a YMM register
        # stops a CPU without AVX (tools/check_isa.py). The prebuilt library of an install was checked when it was made.
        import check_isa
        rt_objs = [o for o in objs if o.suffix == '.o' and 'gen' not in o.relative_to(out / 'obj').parts[:1]]
        fail = check_isa.check_objects(rt_objs, check_isa.find_objdump(bin_dir / 'llvm-objdump.exe'),
                                       out / 'obj' / 'rt' / 'isa-v2.json')
        if fail:
            target.unlink(missing_ok=True)   # not an exe or library that crashes such a CPU
            raise SystemExit(f'build.py: {fail}')
    sys.exit(code)


if __name__ == '__main__':
    main()
