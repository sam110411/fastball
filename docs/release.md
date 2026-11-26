# Releases: Fastball, the setup and launcher

A release is a setup, not the game. It turns the player's own game files into a playable folder on their PC, the way
TotK Recomp, Astral Chain Recomp, Super Odyssey and Savage Wind publish theirs. **The release holds no game code,
data or assets, and no file built from them**: no `mlb.exe`, no generated C or its objects, no shader translations.
`mlb.exe` contains the recompiled game code, so only the player's PC ever builds it, from their files.

## What the player does

1. Downloads `Fastball-<version>-windows.zip` from GitHub Releases, unpacks it anywhere and starts
   `Fastball.exe`.
2. The window is already filled in when it can be: `prod.keys` from Ryujinx's or yuzu's (and forks') folder, the base
   game and update NSPs (1.0.20 preferred, else 1.0.15) from the emulators' game folders, Downloads, Desktop, Documents, `~/Games` or the folder
   around the setup (`installer/detect.py`), and the install folder `MLBTS\` inside the setup folder (on the disk
   with the most free space instead when the setup's disk lacks the room). They check it, change what is wrong and
   press **Install**. Options: shortcuts (off); NVIDIA DLSS (on a GeForce RTX) and AMD FSR (on any AMD, NVIDIA or Intel card) are
   downloaded without asking for the graphics card the setup finds,
   copy the Ryujinx save (on when Ryujinx has one and the install has none), Road to the Show equipment offline
   and Road to the Show packages offline (both off; the launcher screen can add or remove them later).
3. Waits about 10 minutes (on the dev PC: 4 for the 37 GB of game data, 1 to lift, 3 to compile).
4. Presses **Play**. Next time, `Fastball.exe` starts the game at once, without a window. Its launcher screen
   (Play, Update, Repair, Import Ryujinx save, Open install folder) opens when there is something to do (a rebuild, a
   newer release on GitHub), with `--menu`, or with Shift held while it starts.

The base and update can be picked the wrong way round: the setup swaps them.

Updating the port (from 0.1.1): the launcher asks GitHub for the latest release (`installer/update.py`; one API
request, nothing about the player is sent, a failure is silent): when its screen opens, and before it starts the game
straight away at most once every 20 hours, waiting 3 s at most (the answer is kept in `launcher.json`). When that
release is newer than the setup, the screen shows **Download and update**: the setup zip is downloaded, checked against
the SHA-256 GitHub lists for the asset, and unpacked into `.update\Fastball-<version>\` in the setup folder. The
window quits and that release's own Python swaps it in (`python -m installer.update --apply`): it moves the release's
files (`layout.RELEASE_ENTRIES`) over the old ones, which go to `.update\old-<time>\` and come back if anything
fails, then starts the new `Fastball.exe` with `--update <install folder>`, which rebuilds `mlb.exe` at once. The
game files, saves, settings and `launcher.json` stay; the game files are not needed again; the next window removes
`.update\`. The manual way still works: download the new setup, start it, press **Update**. Setup 0.1.0 has no
updater, so its players update by hand once. Setups 0.1.1 to 0.1.5 unpack the update beside their folder (their own
updater): the new setup then keeps working from that folder with the old install folder.

The name: up to 0.1.5 the launcher was `MLBTS-Setup.exe`, the release `MLBTS-Setup-<version>-windows.zip` and the
install record `mlbts-install.json`; since the project is called Fastball they are `Fastball.exe`,
`Fastball-<version>-windows.zip` and `fastball-install.json`. What keeps the older ones working: each release also
carries `MLBTS-Setup-<version>-windows.zip` (the same folder plus a copy of the launcher named `MLBTS-Setup.exe`), the
only zip the updaters of 0.1.1 to 0.1.5 find and the only exe they start; the window then deletes that copy. The
updater takes either zip, Fastball's first, and an in-place update removes an old `MLBTS-Setup.exe`
(`layout.LEGACY_ENTRIES`). An install's `mlbts-install.json` is read when there is no `fastball-install.json` and
replaced by it at the next setup run. The install folder `MLBTS\` (the game's name) stays. The GitHub
repository is `sam110411/fastball` since 0.2.0; the updaters of 0.1.x still ask for `sam110411/mlbts-native`, which
GitHub redirects.

## Portable and one executable

The port keeps to the folder the player unpacked (after RecompLauncher's call for shared standards of recomp
projects):

- The player runs one program, `Fastball.exe`: the setup the first time, the game afterwards (it starts the built
  `mlb.exe` without a window). Shortcuts, when ticked, point at it too.
- The game goes into `MLBTS\` in the setup folder (RecompLauncher's place for it as well). The setup's memory
  (`launcher.json`), its logs (`logs\`), its temporary build files (`temp\`, removed after the build) and updates
  (`.update\`) are in the setup folder; `mlb.exe` keeps its settings, logs, saves and caches beside itself. Deleting
  the folder removes the port. Nothing is written elsewhere unless the player asks: desktop and Start menu shortcuts
  (off by default), or an install folder they pick on another disk. Setups up to 0.1.5 remembered the install in
  `%LOCALAPPDATA%\MLBTS-Native\setup.json`; a newer setup reads it once and removes it.

## Command line

Every choice is a flag, so launchers and scripts need no window:

- `Fastball.exe [--menu] [--dest DIR] [game flags]`: starts the game (or the window, `installer/gui.py`); every
  argument it does not know goes to `mlb.exe`.
- `mlb.exe --help` lists the game's flags: `--game DIR` (or `--exefs`, `--romfs`), `--profile DIR`, `--fullscreen`,
  `--windowed`, and every setting of `mlb.cfg` as `--<setting> VALUE` (`--fps-limit 120`, `--dlss quality`, `--hdr`,
  `--no-rumble`) or `--set KEY=VALUE`. A flag is for that run: it beats `mlb.cfg` and the environment and is not
  written into `mlb.cfg`. `mlb.exe` is a windowed program: its answers go to the console it was started from, else to a
  message box.
- `python\python.exe -m installer --help`: the setup without a window (`installer/cli.py`): `--dest`, `--base`,
  `--update`, `--keys`, `--titlekeys`, `--temp` (the build folder), `--dlss`, `--shortcut`, `--import-save`, the Road
  to the Show options, `--cpu auto|v3|v2` (the build's instruction set; auto reads the CPU), `--launcher`.

## The install folder

```
<install folder>/                default MLBTS\ in the setup folder
  mlb.exe, SDL3.dll              the game, built on this PC (tools/build.py --portable: paths beside the exe)
  totk_xlate.exe, totk_xlate.id  the shader translator
  nvngx_dlss.dll                 NVIDIA DLSS, downloaded from NVIDIA's DLSS repository on a GeForce RTX
  amd_fidelityfx_*_vk.dll        AMD FSR 3.1 / 4 (loader and upscalers), from AMD's FSR SDK 2.4.0 on any AMD, NVIDIA or
                                 Intel card (installer/steps.py FFX_DLLS); --no-dlss / --no-fsr skip, --dlss / --fsr force
  mlb.cfg                        settings; the setup writes MLB_SAMPLER=off (the profiler is off by default since 0.1.4 anyway)
  game/exefs/, game/romfs/       extracted from the player's NSPs (+ romfs_list.tsv, romfs_info.json, romfs.complete)
  profile/                       saves, account
  logs/                          mlb.log of the last run
  shader-cache/, pipeline-cache.bin   made while playing
  equipment-pictures/            the item pictures of the equipment option, when it is on
  packages/                      the local server of the packages option and the Python files it runs on, when it is on
  fastball-install.json          what built the folder: release, finished steps (mlbts-install.json up to 0.1.5)
```

## Setup steps

`python -m installer` (`installer/cli.py`, `installer/steps.py`) prints one JSON event per line; the window
(`installer/gui.py`) runs it as a child process and draws its events. Each step records a key in
`fastball-install.json`; a step with the same key and its output in place is skipped, so a cancelled setup resumes and an
update only rebuilds.

1. **Check**: the setup folder is complete; the CPU's level (`installer/detect.py cpu_level`: a probe of
   `runtime/core/cpulevel.h`, compiled with `llvm/` and run, reads CPUID): x86-64-v3 (AVX2, FMA, BMI2) builds the
   normal game, x86-64-v2 (SSE4.2, POPCNT) a slower one for CPUs without AVX2, anything older is refused; a Vulkan
   driver is installed; the NSPs are MLB The Show 26 and its 1.0.20 or 1.0.15 update (`cnmt.xml` in the NSP, else the
   ticket's name);
   `prod.keys` has a `header_key`; free space (37 GB less what is already extracted, plus 2 GB; 2 GB next to the
   setup for the build).
2. **ExeFS**: the update's code into `game/exefs`, checked against `tools/data/expected-<version>.json`. The
   version is the picked update's, else the installed code's, else the newest (`Context.pick_version`).
3. **RomFS**: `tools/romfs_dump.py --extract-all --progress`: base plus update, IVFC-checked (a failure stops the
   setup: damaged files), resumable. `game/romfs.version` names the update the data belongs to (none: 1.0.15, from
   setups before 1.0.20 support); another version's data is removed first, because the extraction skips files that
   are there at their size.
4. **Build**: `recomp/gen.py` into the setup's `temp/`, then `tools/build.py --toolchain llvm --runtime-lib
   runtime/libmlbrt.a --cpu v3 --portable --exefs game/exefs` (the game version comes from main's hash; on a CPU
   without AVX2 `runtime/libmlbrt-v2.a --cpu v2`, and the step's key holds the level, so the same folder on another
   CPU builds again); `mlb.exe`, `SDL3.dll` and the translator are copied into the install folder. `mlb.exe` checks
   the CPU before `main` (`runtime/mlb/cpucheck.c`) and says to run Repair when it was built for a level the CPU lacks.
5. **Equipment** (only with `--equipment`, or on an install that has it): `tools/equipment/equipment.py install`
   adds every Road to the Show equipment item to the game data, with pictures rendered from the player's files
   ([tools/equipment](../tools/equipment/README.md)). `--no-equipment` puts the game data back. An install that has
   it keeps it through updates and repairs.
6. **Packages** (only with `--packages`, or on an install that has it): copies `tools/packages` and the files of
   the setup's Python that it needs to `packages/`, runs `server.py --check` there (which also makes this PC's
   certificate) and writes `MLB_OFFLINE_PACKAGES=1` into `mlb.cfg`
   ([tools/packages](../tools/packages/README.md)). It brings the equipment step with it, with the equipment not
   owned at the start. `--no-packages` removes the folder and the setting; the equipment then goes back to owned.
7. **Finish**: `mlb.cfg` (when missing), DLSS and FSR for the graphics card (every setup and update looks again), shortcuts (PowerShell's `WScript.Shell`; to `Fastball.exe`, only when asked), `mlb.exe --import-save`,
   and `temp/` deleted.

Logs: `logs/setup-<time>.log` in the setup folder. The keys file's path is passed to the tools; its contents are never
logged, stored or shown.

## Why llvm-mingw

The dev build targets MSVC and links Visual Studio's libraries, which may not be given to players. The release ships
llvm-mingw (Apache-2.0 with LLVM exception; mingw-w64's permissive licences), which compiles and links on the
player's PC with nothing installed. `tools/build.py --toolchain <llvm-mingw>` builds the same sources for
`x86_64-w64-mingw32`; libc++, libunwind and winpthread are linked statically. Measured on the dev PC (Ryzen 5 5600X,
RTX 3060 Ti), boot to the menus: 60 fps in both builds, same render and replay times. Only `prof_win.c` needed a change:
llvm-mingw has no `__try`, so its two guarded unwind calls use a vectored handler there (`GUARDED`).

## Release layout

```
Fastball.exe        installer/launcher/launcher.c: starts python\pythonw.exe -m installer.gui, no console
python/             python.org embeddable 3.14.9, numpy, lz4 (tools/requirements-release.txt), tkinter
llvm/               llvm-mingw 20261020, x86_64 subset
runtime/            libmlbrt.a and libmlbrt-v2.a (the runtime for x86-64-v3 and v2, built with llvm/), every runtime
                    header, deps/ (Vulkan headers, VMA, SDL3
                    headers, lib/vulkan-1.lib, bin/SDL3.dll)
bin/                ninja.exe, totk_xlate.exe (+ .id)
recomp/, tools/     the lifter; build.py, romfs_dump.py and its imports, data/expected-1.0.15.json and -1.0.20.json,
                    equipment/ and packages/ (the optional equipment and packages steps)
installer/          the setup
licenses/, LICENSE, THIRD_PARTY_NOTICES.md, README.txt, VERSION (VERSION plus the commit)
```

## Making a release

Once per machine: `tools/fetch_deps.py`, `tools/build_xlate.py`, and `recomp/gen.py` (the runtime library reads the
shared headers of `work/gen`, never its C). Set `VERSION`, commit, then:

```
.venv/Scripts/python -I tools/make_release.py
```

It builds from a clean export of the last commit (`git archive HEAD` into `work/release-src`): uncommitted work is
never in a release and the working tree is left alone (`--from-worktree` builds from it instead, for tests only). It
downloads the pinned files (TotK Recomp's pins, SHA-256 checked, cached in `work/toolchain/release-dl`), builds
`libmlbrt.a`, `libmlbrt-v2.a` (checked by `tools/check_isa.py`) and `Fastball.exe` with the release's own
llvm-mingw, and refuses the folder if any file carries game
data, the generated-code marker, key lines, or is an executable outside the tools it lists. Then the folder's own Python
imports the setup, the window builds every screen (`--selftest`), and a setup run without game files must stop asking
for them. The zips are `work/release/Fastball-<version>-windows.zip` and `work/release/MLBTS-Setup-<version>-windows.zip`
(the same plus `MLBTS-Setup.exe`, for the updaters up to 0.1.5): publish both.

Before publishing, install from the unpacked release folder once (the setup's full path with the real files), play a
little and check `logs/mlb.log`. Publishing: a GitHub release with a `v<version>` tag, the zip as its asset, and notes
that say no game files are included (`docs/release-notes-<version>.md`).

## Not yet

- XCI and NSZ input: `tools/romfs_dump.py` reads NSP files only.
- Preparing the shaders at install: the game translates them while playing, so the first matches stutter while the
  cache fills.
- One language (English) in the setup window.

## RecompLauncher

[RecompLauncher](https://recomplauncher.com) installs and updates the port from the GitHub releases. `recomplauncher.json`
at the repository root is copied into each release (`tools/make_release.py`) and tells it how:

- Easy Setup asks for the base NSP, the update NSP and prod.keys, then runs the setup without a window:
  `python/python.exe -m installer --base ... --update ... --keys ... --dest "{install}/MLBTS" --launcher`.
- `--launcher` prints `progress <0..1> <text>` lines instead of the JSON events (`installer/events.py` ProgressLines), ends
  the tools if the launcher stops the setup (`installer/cli.py`), and allows the install folder inside the setup
  folder: the launcher unpacks each release over its folder and never touches `MLBTS/`.
- After an update the launcher runs the setup again; it skips the extraction and rebuilds `mlb.exe`.
- The launcher's settings tab writes `MLBTS/mlb.cfg` (the keys of `runtime/mlb/cfg.c`), and backs up `MLBTS/profile`.
  Its keys are spelled `MLB_<KEY>`, as the in-game menu writes them (`cfg.c` save_file): the game reads `X` and
  `MLB_X` as one setting, but the launcher matches the text, so with `X` it would not see the menu's `MLB_X` line,
  and would add its own line after it that undoes the menu's change.

When a setting is added to or renamed in `cfg.c`, update its entry in `recomplauncher.json` too.
