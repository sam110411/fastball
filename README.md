# Fastball: MLB The Show 26, native on Windows

Fastball is a native Windows port of MLB The Show 26 for Switch.

<a href="https://recomplauncher.com/game/mlbts-native"><picture><source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg"><img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48"></picture></a>

A static recompilation of **MLB The Show 26** (Nintendo Switch, version **1.0.20**; 1.0.15 also works) into a native Windows x86-64
program. The game's ARM64 code is translated to C ahead of time, compiled with clang, and linked against a small
host runtime that stands in for the Switch's operating system, GPU API and audio.

![Pitching screen](docs/img/pitching-screen.jpg)

> **Fan project.** This project is not affiliated with, endorsed by, or connected to Sony Interactive Entertainment,
> San Diego Studio, MLB, or Nintendo. It contains no game code, game assets or keys. You must supply your own legally
> obtained copy of the game.

## Status

Playable offline. The game boots, the menus work, and Exhibition, Road to the Show and Franchise run at 60 fps on the
development machine (Ryzen 5 5600X, RTX 3060 Ti, Windows 11).

**Scope:** offline play only (exhibition, Road to the Show, Franchise, menus, local saves). Online play and Diamond
Dynasty are out of scope.

**Known problems:** current work is graphics correctness (some wrong or missing images, lighting) and performance.
Game versions 1.0.20 (the latest) and 1.0.15 are supported; other versions are not.

## Playing it

1. Download `Fastball-<version>-windows.zip` from [Releases](https://github.com/sam110411/fastball/releases),
   unpack it where you want the game to live (it needs about 40 GB there) and start **`Fastball.exe`**.
2. The setup looks for your game files and keys by itself (Ryujinx's and yuzu's folders, Downloads, Desktop, ...).
   Check what it found, pick anything missing, and press **Install**.
3. After about 10 minutes, press **Play**. From then on, `Fastball.exe` starts the game straight away (hold Shift
   while starting it for its window: updates, repair, save import).

Everything stays in that folder: the game goes into `MLBTS\`, and deleting the folder removes the port. Flags work too:
`Fastball.exe --fullscreen --fps-limit 120` (`MLBTS\mlb.exe --help` lists every setting).

The download contains no part of the game. The setup builds the game on your PC from your own files: the base game and
the 1.0.20 update (or 1.0.15) as NSP files dumped from your Switch, and your `prod.keys`. It needs about 40 GB free. To update the
port, press **Download and update** when the setup offers it (it checks once a day), or start a newer setup and press
**Update**; your game files, saves and settings stay. How the setup works and how a
release is made: [docs/release.md](docs/release.md).

## Features

- Native Windows executable, Vulkan 1.3 renderer. Switch shaders are translated at run time.
- Switch saves are used as they are: a Ryujinx save can be imported (the setup offers it, or `mlb.exe --import-save`).
- Road to the Show equipment without a connection, as an option of the setup: every item, with its picture, owned
  from the start ([`tools/equipment`](tools/equipment/README.md)).
- Road to the Show's Packages without a connection, as another option of the setup: the game earns and opens its
  equipment packs against a small server on the player's own PC ([`tools/packages`](tools/packages/README.md)).
  Nothing is sent anywhere, and it is off unless turned on.
- Xbox and PlayStation controllers (through SDL3) with rumble, one player per controller (up to 4) for local
  multiplayer. The game's button prompts follow player 1's controller.
- Keyboard and mouse.
- Resolution options: output size, render scale, FSR 1, NVIDIA DLSS / DLAA and AMD FSR 3.1 / FSR 4 (the setup
  downloads the DLSS and FSR libraries for the graphics card from NVIDIA's and AMD's repositories; they are not
  shipped here).
- FXAA, anisotropic filtering, shadow quality options, colour presets, HDR output, field-of-view and aspect options.
- In-game settings menu, performance overlay, fullscreen toggle (F11).
- Frame rate limit from 30 to 240 fps or the monitor's rate, at the game's normal speed
  ([docs/frame-rate.md](docs/frame-rate.md)).
- Software-mixed 5.1 audio output through WASAPI.
- Built-in diagnostics for graphics bugs: frame capture (F12), draw logs, RenderDoc support.

Every setting is a `KEY=VALUE` line in `mlb.cfg` next to `mlb.exe` (or an environment variable named `MLB_<KEY>`). The
full list is the table in [`runtime/mlb/cfg.c`](runtime/mlb/cfg.c).

## System requirements

| | Minimum |
|---|---|
| OS | Windows 10/11, 64-bit |
| CPU | x86-64 with SSE4.2 (Intel Core i 1st generation, AMD FX or newer); AVX2 recommended: without it the setup builds a slower version for the CPU |
| GPU | Vulkan 1.3 capable (an NVIDIA RTX card is needed for DLSS) |
| RAM | 16 GB recommended; the page file should be large (RenderDoc captures need much more) |
| Disk | About 40 GB for the extracted game data, plus the build |

## Game files

Nothing from the game is included. You need:

- the base game and the 1.0.20 update (or 1.0.15), dumped from your own Switch (`.nsp` files), and
- your own `prod.keys` and `title.keys`. By default the tools read them from `%APPDATA%/Ryujinx/system/`. They are
  passed by path and never printed or copied.

Dumps, extracted data and builds live in `gamefiles/` and `work/`, which are git-ignored. Never commit them.

## Building from source

This section is for development. To play, use the setup above.

Requirements: LLVM clang with lld, Ninja and Python (a `.venv` in the repository), .NET 10 with Visual Studio 2022's
linker (for the shader translator). Always run Python as `.venv/Scripts/python -I <script>`; `-I` keeps user
site-packages from loading.

First-time setup, once:

```bash
# 1. extract the ExeFS and the RomFS from your own dumps (about 37 GB)
.venv/Scripts/python -I tools/romfs_dump.py --keys "$APPDATA/Ryujinx/system/prod.keys" --titlekeys "$APPDATA/Ryujinx/system/title.keys" --extract-all "gamefiles/<base>.nsp" "gamefiles/<update>.nsp" work/romfs
# 2. generate C from the game's code, with its code patches (about 50 s)
.venv/Scripts/python -I recomp/gen.py --exefs work/update/exefs --out work/gen --modules main,sdk --patches tools/data/patches-1.0.20.txt
# 3. fetch Vulkan headers, VMA and SDL3; build the shader translator
.venv/Scripts/python -I tools/fetch_deps.py
.venv/Scripts/python -I tools/build_xlate.py
```

Build and run (incremental after the first build, which takes a few minutes):

```bash
.venv/Scripts/python -I tools/build.py
work/build/mlb.exe
```

Details, including how the boot sequence was brought up: [docs/runtime-port.md](docs/runtime-port.md).

## Where files live

| What | Where |
|---|---|
| Executable and settings | `work/build/mlb.exe`, `work/build/mlb.cfg` |
| Log (replaced at each start) | `work/build/logs/mlb.log` |
| Saves and captures | `work/profile/` |
| Extracted game data | `work/romfs/`, `work/update/exefs/` |

Attach `mlb.log` to bug reports. Lines starting with `[lm 0]` are the game's own log, `[host]` lines are ours. A crash
prints `FATAL:` with host frames that can be symbolized with `llvm-symbolizer`.

## Debugging graphics

- **F12** captures a frame (draw log, screenshot, every render target) at no cost until pressed.
- Under RenderDoc (`tools/renderdoc.cmd`) F12 is RenderDoc's capture and ours moves to F9. Images and draws are named
  with their Switch memory address, so a wrong picture can be traced to the draw that made it.
- More: [AGENTS.md](AGENTS.md), [docs/graphics-plan.md](docs/graphics-plan.md).

## Repository layout

| Path | What |
|---|---|
| `recomp/` | ARM64 to C generator |
| `runtime/core/` | Generic host: CPU, memory, kernel and threads, SVCs, IPC, file system, audio, Win32 platform |
| `runtime/mlb/` | MLB layer: boot, settings, services, input, log and crash reporter |
| `runtime/gfx/` | NVN front end, Vulkan back end, presentation, window, in-game menu, RenderDoc support |
| `tools/` | Build, extraction and analysis scripts |
| `docs/` | Binary reconnaissance, runtime notes, graphics plan |

Changes to vendored upstream files are marked `fastball` in the source with a comment explaining the case that
needed them.

## Credits and reference projects

This port stands on the work of the Switch recompilation community. Its generator and runtime come from:

- **[TotK PC Recomp](https://github.com/valentderah/totk-pc-recomp)** (Valiantsin Dzerakh), commit `ae30a61`: the
  code generator, the host runtime core, the NVN and Vulkan graphics stack, audio renderer, DLSS/FSR and in-game menu.
  This is the primary base.
- **[Astral Chain PC Recomp](https://github.com/captainj-sparrow/astral-chain-pc-recomp)**: the `audout:u` service.
- **[Super Odyssey PC Recomp](https://github.com/Simon3Sarkis/super-odyssey-pc-recomp)** and
  **[Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp)**: references for structure and the menu and
  performance overlay.
- **[LAS Recomp](https://github.com/peiikai/LASRecomp)** (Link's Awakening): the original lineage of the Vulkan
  back end and of this kind of project.
- **[Ryujinx](https://github.com/Ryujinx/Ryujinx)**: its shader translator, and a correctness oracle for the game's
  behaviour.
- **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**: read for how NCA storage is layered (no code copied).
- **[RenderDoc](https://github.com/baldurk/renderdoc)**: the main graphics debugging tool.

Libraries: SDL3, Vulkan-Headers, Vulkan Memory Allocator, Dear ImGui, AMD FidelityFX FSR 1, NVIDIA NGX (DLSS, not
redistributed), Roboto font. Exact versions, licences and the files they went into are in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

Third-party code keeps its own licence and attribution header; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The code adapted from TotK Recomp is MIT-licensed
(`runtime/core/LICENSE.totk-recomp`). No game content is distributed with this repository.
