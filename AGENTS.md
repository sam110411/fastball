# AGENTS.md — Fastball, the MLB The Show 26 native Windows port

Native Windows x86-64 static recompilation of **MLB The Show 26 for Switch, version 1.0.20** (the dev tree's version; 1.0.15 stays supported, see "Game
versions" below; no other versions).
Goal: offline play (exhibition, Road to the Show, Franchise, menus, local saves). Online and Diamond Dynasty are out of
scope. Status: boots, menus, RTTS and full matches run at 60 fps on the dev machine (Ryzen 5 5600X, RTX 3060 Ti,
Windows 11). Most current work is graphics correctness (wrong or missing images, lighting) and performance.

## How the user works (read first)

- The user tests in the game themselves and reports what they see; they launch `work/build/mlb.exe` directly.
  Don't launch long runs unasked. Short smoke runs (start, check the log, kill) are fine.
- **Performance comes first.** The user notices any fps loss. Diagnostics must cost nothing during normal play: gate
  them behind F12, a cfg knob or RenderDoc. Remove test code that slows the game once it has done its job.
- They want real fixes backed by evidence, not rounds of guesses. Trace the cause (RenderDoc, logs), then fix.
- Commit normal development to `dev` and push it to GitHub (`origin`, https://github.com/sam110411/fastball); not to
  `main` unless the user asks. Commit only the
  files of the task; the user may have uncommitted work of their own in the tree. End each commit message with the
  `Co-Authored-By:` line the session gives.
- Write explanations in plain language. The user is not a graphics programmer.

## Repository and asset safety (never break)

- Never commit copyrighted game files, extracted game assets, encryption keys, or other sensitive material.
- Keep all game dumps and local extraction artifacts outside version control (`gamefiles/`, `work/`, `refs/` are
  gitignored). Add `.gitignore` rules before generating large amounts of extracted data.
- Do not upload proprietary game files to external services.
- Keys are the user's own: `%APPDATA%/Ryujinx/system/prod.keys` and `title.keys`. Pass their paths to tools and never
  print key values.
- Keep the project buildable and its research reproducible without distributing the original game files.

## Layout

| Path | What |
|---|---|
| `recomp/` | ARM64 → C generator (from TotK Recomp `ae30a61`, unmodified) |
| `runtime/core/` | Generic host: CPU, guest memory, kernel and threads, SVCs, IPC, fs, audio, Win32 platform. Nothing MLB-specific |
| `runtime/mlb/` | MLB layer: `main.c` (log, crash reporter), `boot.c`, `cfg.c` (all settings), `services.c`, `hid.c` (input) |
| `runtime/gfx/` | NVN front end and Vulkan back end (from TotK), present, window, RenderDoc support (`rdoc.c`) |
| `tools/` | Build, extraction and analysis scripts (below) |
| `installer/` | The player's setup and launcher (`Fastball.exe`): extracts their files, builds `mlb.exe` on their PC (`docs/release.md`) |
| `docs/` | `recon-report.md` (binary facts), `runtime-port.md` (build and boot), `graphics-plan.md` |
| `work/` (ignored) | `gen/` generated C, `build/` exe and logs, `profile/` saves and captures, `romfs/`, `scratch/` |
| `refs/` (ignored) | Reference checkouts: TotK Recomp (primary), Astral Chain Recomp, Power Moon Recomp |

Changes to vendored TotK files are marked `fastball` with a comment saying what MLB does and why. Keep that style:
comments explain the observed case (screen, texture, address) that made the code necessary.
Third-party code goes in `THIRD_PARTY_NOTICES.md`.

## Build and run

Run Python as `.venv/Scripts/python -I <script>`. `-I` matters: user site-packages must not load.

```
.venv/Scripts/python -I tools/build.py          # incremental build of work/build/mlb.exe (clang + lld + ninja)
work/build/mlb.exe                               # paths default to work/update/exefs, work/romfs/romfs, work/profile
```

The runtime is built with a profile (`tools/data/runtime.profdata`, profile-guided optimisation; `--pgo off` without
it). Retrain it after large replay changes (`tools/pgo.py`, `docs/runtime-port.md`); the game's code never gets one.
First-time setup (only for a fresh checkout): see `docs/runtime-port.md`. In short: extract the ExeFS and RomFS with
`tools/romfs_dump.py` and the keys, then `recomp/gen.py`, `tools/fetch_deps.py` and `tools/build_xlate.py`.
`mlb.exe` is locked while the game runs: the link fails then, so check `tasklist` first.
CPUs without AVX2: `tools/build.py --cpu v2 --out work/build-v2` builds runtime and game code for x86-64-v2 (SSE4.2);
the default is v3 (AVX2, FMA, BMI2). The setup picks the level from CPUID (`installer/detect.py` `cpu_level`) and links
`libmlbrt.a` or `libmlbrt-v2.a`; `mlb.exe` checks the CPU before `main` (`runtime/mlb/cpucheck.c`). Runtime code must
compile at both levels: AVX intrinsics, `target(...)` attributes or inline AVX assembly need an `#ifdef __AVX__`
SSE path or a runtime CPU check; a v2 build refuses objects with instructions above v2 (`tools/check_isa.py`).
Upscalers: `MLB_DLSS` needs `nvngx_dlss.dll` and `MLB_FSR` (FSR 3.1 / 4, `vk_ffx.c`) AMD's two DLLs next to `mlb.exe`;
`tools/fetch_ffx.py` puts AMD's into `work/build`. Both are off by default and turned on in the F1 menu (Graphics).

Settings: `work/build/mlb.cfg`, one `KEY=VALUE` per line. Environment variables `MLB_<KEY>` (or `TOTK_<KEY>`) override
it. The full list is the table in `runtime/mlb/cfg.c`; `main.c` turns `FRAME_QUEUE` and `REPLAY_ASYNC` on by default.
With async replay, the NVN command replay runs on its own thread, and a recorder thread makes the Vulkan commands
(`vk_rec.c`).

For edits across several files, a small script in `work/scratch/edit_*.py` using exact-match
replace-and-assert is reliable.

## Game versions

- Supported: **1.0.20** (update v1310720, the dev tree's: `work/update/exefs`, `work/romfs`) and **1.0.15** (v983040,
  kept in `work/update-1.0.15/exefs` and `work/romfs-1.0.15`). Each has `tools/data/expected-<version>.json`.
- One `mlb.exe` runs one version. `tools/build.py` picks it from the SHA-256 of `--exefs`'s `main` (default
  `work/update/exefs`) and refuses `--gen` output of another version. The runtime reads it as `rt_game_build`
  ("1.0.20"). To build 1.0.15: `recomp/gen.py --exefs work/update-1.0.15/exefs --patches
  tools/data/patches-1.0.15.txt`, then `tools/build.py --exefs work/update-1.0.15/exefs` and `MLB_EXEFS`/`MLB_ROMFS`
  pointing at the 1.0.15 folders. Each version's code patches (`tools/data/patches-<version>.txt`, the
  `recomp/patches.py` format) go to `recomp/gen.py --patches`; the setup passes them too.
- 1.0.20 changed only `main` (sdk, rtld and the 765 imports are the same). Code addresses moved by +0x20 to +0x1500,
  read-only data and data by about +0x1000 to +0x1070. Address comments in `runtime/` are 1.0.15's unless they say
  otherwise; `tools/addrmap.py work/update-1.0.15/exefs work/update/exefs <addr>...` finds them in 1.0.20. Code that
  uses a game address picks it per version (`resolution.c`).
- New in 1.0.20: the game modes menu asks the eShop for consumables (`nim:IShopServiceAccessServer`, refused offline
  in `services.c`). Its RomFS has 1,601 more files (rosters, heads, stadiums) and 189 of 468 shader banks rebuilt.

## Releases

A release is a setup, never the game: **no `mlb.exe`, generated C, objects or anything else built from the game's code
or data** goes into it. The player's PC extracts their own NSPs and builds `mlb.exe` with the bundled llvm-mingw and the
prebuilt runtime library `libmlbrt.a` (`tools/build.py --toolchain ... --make-runtime-lib`, which contains no game
code; `libmlbrt-v2.a` with `--cpu v2`, for CPUs without AVX2). `tools/make_release.py` builds from `git archive HEAD` (uncommitted work never ships) and refuses a folder that
fails the content check. The dev build stays MSVC; runtime code must also compile with llvm-mingw (no `__try`, no
MSVC-only APIs): check with `tools/build.py --toolchain work/toolchain/llvm-mingw-20261020-ucrt-x86_64 --out
work/build-mingw`. Details: `docs/release.md`.

The release keeps to RecompLauncher's standards for recomp projects (`docs/release.md`, "Portable and one
executable", "Command line"): one program to run (`Fastball.exe` starts the built game without a window), portable
(the game in `MLBTS\` inside the setup folder, the setup's memory in `launcher.json` there, updates swapped in place,
nothing in `%LOCALAPPDATA%` or shortcuts unless asked), and a flag for every choice (`mlb.exe --help`: every `cfg.c`
setting is a flag). A new setting needs no extra flag code, but anything new the setup writes must stay in its folder.

Publishing a version: bump `VERSION`, write `docs/release-notes-<version>.md`, commit and push, run
`tools/make_release.py`, then `gh release create v<version> work/release/Fastball-<version>-windows.zip
work/release/MLBTS-Setup-<version>-windows.zip --target <full commit SHA> --title "Fastball <version>" --notes-file
docs/release-notes-<version>.md` (a short SHA
is refused). Players' launchers (0.1.1 and later) find the newest release through GitHub's API and update from it
(`installer/update.py`): the assets must keep the names `Fastball-<version>-windows.zip` and
`MLBTS-Setup-<version>-windows.zip` (the second, with a copy of the launcher as `MLBTS-Setup.exe`, is the only one the
updaters of setups 0.1.1 to 0.1.5 find and can start). The GitHub repository was renamed from `mlbts-native` to `fastball`
(2026-11-22, before 0.2.0): GitHub redirects the old URLs, so the updaters of 0.1.x (which ask
`repos/sam110411/mlbts-native`) still find new releases. Never create a repository named `mlbts-native` under the same
account: that would end the redirect.

Nightly builds (a snapshot of `dev` for players to test) are a pre-release under the single `nightly` tag, never
"Latest", with one asset named `Fastball-nightly-<yyyymmdd>-<commit>-windows.zip` and no `VERSION` bump, so the
launchers' updater never offers them: `docs/release.md`, "Nightly builds".

## Logs and crash reports

- `work/build/logs/mlb.log`: the whole run. It is replaced at the next start, so **copy it to `work/scratch/` before
  launching anything** if it holds evidence. `[lm 0]` lines are the game's own log; `[host]` lines are ours.
- Developer diagnostics are off by default (players' builds too); `work/build/mlb.cfg` turns them on for the dev
  tree: `MLB_TRACE=perf,sched`, `MLB_SAMPLER=on`, `MLB_CAPTURE_KEY=1`, `MLB_STATEDUMP=1`.
- Once a second (`MLB_TRACE=perf`): `perf: frame N:` lines (fps, GPU time, texture uploads and aliases, VRAM), `mem:`; `sched:` with `sched`.
  `sched: <thread> wait N: host on exe+0x...` names where a thread is stuck.
- Crash: `FATAL: ...` with host frames like `mlb+0xc0944`. Symbolize them with
  `"/c/Program Files/LLVM/bin/llvm-symbolizer.exe" --obj=work/build/mlb.exe --relative-address 0xc0944`.
- With `MLB_SAMPLER=on`, slow frames (19 ms+) dump a sampler profile to `work/profile/prof.txt.f<frame>`. Read it with
  `.venv/Scripts/python -I tools/prof_report.py <profile> [map] --top 40 --chains 30`.

## In-game diagnostic keys

- **F12** (no RenderDoc, with `MLB_CAPTURE_KEY=1`): our own capture, 2 frames ahead, with a beep. It writes:
  - the frame's draw log into `mlb.log` (`drawlog` lines: every draw's pass, formats, shader hashes, textures and
    their guest addresses);
  - `work/profile/shot-<frame>.png`, the presented picture;
  - `shot-<frame>-<n>-<w>x<h>-fmt<nvn>.png`, every render target of the frame;
  - `shot-2<frame>-*`, textures noted for diagnosis;
  - `verify:` lines comparing large drawn textures with their guest memory.
  It costs nothing until pressed.
- **F11**: fullscreen toggle.
- **F8** (only with `MLB_STATEDUMP=1`): writes main's data and bss to `work/profile/state-<frame>.bin` with a beep, to
  compare game variables between situations (`statedump.c`).
- **Under RenderDoc**, F12 is RenderDoc's capture (see below) and our capture moves to **F9**.
- Knobs for the same machinery: `MLB_DRAWLOG=<frame>|game`, `MLB_SHOT=<frame>,...`, `MLB_DRAWTEX=<fs hash>,...`
  (save the textures of draws with those fragment shaders), `MLB_WATCH=<addr>,<size>;...` (log texture creations and
  GPU writes in a guest range), and `MLB_TRACE=svc,host,ipc,fs,gfx,perf,sched,audio,present,all`.
- Following game code without symbols (slows the game while set; nothing when unset):
  - `MLB_TRACE=fs` logs every open (and failed open), the first reads and every 64th of each file, and closes with
    totals; with `MLB_FS_STACK=<path text>` also the game's call stack at each open, read and close of matching files.
  - `MLB_FNTRACE=<addr>,...` logs calls of game functions (caller, x0; x0 shown as text when it points at a string),
    `<lo>-<hi>` the first call of every function in a range; `MLB_FNTRACE_PEEK=<off>,...` logs a value behind x0
    each time it changes (pointer chain, last read as 32 bits). The engine's print function (1.0.20 `0x7001ad9090`, 1.0.15 `0x7001ad7bd0`) shows its
    messages this way. Function addresses are the `f_<addr>` names in `work/gen/main/*.c`.

## RenderDoc (the main graphics debugging tool)

- RenderDoc 1.43: `C:\Users\Ariel Osvaldo\tools\renderdoc-1.43\x\RenderDoc_1.43_64`. The **renderdoc MCP server** is
  configured (`mcp__renderdoc__*` tools: open_capture, list_draws, list_events, pixel_history, debug_pixel,
  get_resource_usage, export_texture, export_render_target, ...).
- The user launches `tools/renderdoc.cmd`. It runs the game under `renderdoccmd` with a 2 GB soft memory limit, and
  captures go to `work/scratch/rd/mlb_frame<N>_capture.rdc`. **F12** captures one whole game frame: from a present to
  the first present after game work (`rdoc.c`; menus present some frames without drawing). `MLB_RDOC_FRAME=<frame>`
  captures without a key, which is useful for tests. The MCP's `capture_frame` also works, but it reports "file not
  found" because of the frame suffix; the file is still there.
- Under RenderDoc the port:
  - uses shadow pool storage, because RenderDoc hides `VK_EXT_external_memory_host` (slower, same content);
  - names every image `tex <guest addr> fmt <nvn fmt> WxH [mips] [rt]` and pool buffers `pool <addr>+<size>`;
  - puts a label before each draw: `<kind> <count> rt0 <addr>: t<binding> <addr> ...` (the fragment textures it
    samples). Use `list_events` for the labels, and `list_resources` with a name filter (e.g. `tex 0x18b000000`).
- Tracing a wrong pixel:
  1. Find the final target (`list_resources` name `1920x1080`, then `get_resource_usage`).
  2. `pixel_history` on the bad pixel finds the draw that wrote it.
  3. `debug_pixel` with `mode=trace` shows the sampled values. The output is huge: it lands in a file, so condense it
     with `.venv/Scripts/python -I tools/rd_trace.py <file> 40`.
  4. The draw's label gives the texture address; `export_texture` shows its contents.
- Memory: a capture needs several GB of commit. The page file is now 32 GB (with 1 GB, RenderDoc crashed at F12). The
  MCP process holds about 6 GB while a big capture is open: afterwards open the small
  `work/scratch/rd/test900_frame955.rdc` to release it. If RenderDoc crashes, its log is in `%TEMP%\RenderDoc\`.

## Graphics architecture notes (where bugs have been)

- Switch textures live in guest memory (block-linear tiling). The port keeps a texture cache (`vk_texture.c`) keyed
  by the full descriptor, plus a list of entries per address (`by_addr`).
- Several images often share one address. On the console an image *is* its memory: a render into one replaces the
  bytes of the others there. The port has a VkImage per descriptor. So aliasing is the most common source of "images
  from another menu":
  - `alias_copy` (a new entry takes a live entry's GPU content);
  - `alias_overwritten` (an entry another image rendered over takes or clears its content);
  - `slot_resolve`: a texture slot follows another-size entry only if that entry holds GPU content (fixed in
    `54e23e8`: badges and the diamond HUD sampled stale menu images).
- MLB compresses some textures on the GPU (BC7 via an R32G32B32A32_UINT render plus a copy: player portraits and
  hairstyles). A CPU rewrite of a GPU-owned texture is caught by `cpu_rewrite_refresh` (the hairstyle fix).
- Pools: on a discrete GPU, CPU-visible pools get device-local mirrors synced from write-watched pages (`vk_memory.c`).
  GPU writes are written back to guest memory at fences.
- Shaders: Maxwell shaders are translated by a .NET tool built from `tools/xlate` (Ryujinx's translator plus TotK's
  patches). Pipelines and shader caches live in `work/build/`.

## Frame rate notes

- MLB steps its game by the measured frame time, not by frames: its frame timer only sleeps to a vsync count the game
  keeps in its data. `MLB_FPS_LIMIT` (F1 → Display) other than 60 sets that count to 0 at each present and the
  present pacer holds the limit (`runtime/mlb/framerate.c`, `docs/frame-rate.md`). No game code is patched.
  `MLB_TRACE=perf` logs game time against wall time (`perf: frame rate:`).
- Heavy scenes (fan cam) are slow because the game waits for each frame's GPU signal before starting the next: game
  code + replay + the GPU's tail add up. What was measured, done and ruled out: `docs/frame-rate.md`, "What limits the
  frame rate in heavy scenes".

## Audio notes

- MLB mixes everything in software (Sony's Scream engine) and sends one 5.1 PCM stream to `audout` (`audout.c`).
  It also opens `audren` but never plays a voice on it.
- Crowd, music, organ, PA and commentary are streams (`NSW/AUDIO/BANK_STREAMS`, `*.SKX`). Scream parses their headers
  through `nn::fs` but opens them for playback with the C library's `fopen`. The SDK's own libc cannot see the port's
  mounts, so `fs_host.c` serves the game's FILE calls (`fopen`, `fread`, `fseek`, ...) on host mounts.
- Guest paths may contain `..` (`FS_Rom:/NSW/../CROSS/STADIUMS/AUDIO/<team>/AUDIOEMITTER.BIN`); `fs_host.c`
  normalises them as nn::fs does.

## Reference projects

Study these before designing major components. They are references, not drop-in solutions: MLB's engine, SDK version,
title IDs and module layout differ.

1. **TotK PC Recomp**, https://github.com/valentderah/totk-pc-recomp (`ae30a61`): the source of recomp/, runtime/core
   and runtime/gfx. Read it first for anything in those trees.
2. **Astral Chain PC Recomp**, https://github.com/captainj-sparrow/astral-chain-pc-recomp: `audout:u` came from it.
3. **Super Odyssey PC Recomp**, https://github.com/Simon3Sarkis/super-odyssey-pc-recomp.
4. **Savage Wind Recomp**, https://github.com/Revivedx/SavageWindRecomp.

Ask before running code from a reference repo, except TotK's lifter, which the user approved as a coverage oracle.
Ryujinx runs the game for the user and can be used as a correctness oracle.

## Engineering guidelines

- Correctness and reproducibility over premature optimization; but never regress the user's frame rate.
- Keep generic runtime code (`runtime/core`) separate from MLB-specific code (`runtime/mlb`, marked game cases).
- Verify against the actual binaries and captures; clearly separate confirmed findings from hypotheses.
- Fix root causes in the port, not symptoms per screen. Implement incrementally; build and smoke-test each change.
- Document significant discoveries in code comments and `docs/`. Avoid unnecessary abstractions.
- It is preferred that you use your edit tool to edit files instead of using commands or creating scripts to edit them.