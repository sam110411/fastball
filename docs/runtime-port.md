# Runtime port: status and layout

How `mlb.exe` is built from the game's code and TotK Recomp's runtime, and how far it boots. Updated 2026-11-08.

## Layout

| Path | What | Origin |
|---|---|---|
| `recomp/` | ARM64 → C generator (lifter, NEON/FP lowering, jump tables, flags/register caching, module layout) | TotK Recomp `ae30a61`, unmodified |
| `runtime/core/` | Generic host: CPU/SIMD headers, dispatch, host-function table, NSO loader, guest memory, kernel/threads (3 virtual cores), SVCs, IPC, platform (Win32), audio-renderer ABI header | TotK Recomp `ae30a61`; Fastball changes listed below |
| `runtime/mlb/` | MLB layer: `main.c` (log, crash reporter, runtime services of the generated code), `boot.c` (rtld replacement, `nn::init::Start`, NPDM service list), `cfg.c` (environment settings), `services.c` (service table; lm, apm, ectx:aw, pctl), `am.c` (appletOE), `aoc.c` (aoc:u), `time.c` (time:u), `game.c` (NACP figures for fs), `account.c` (local user id), `packages.c` (optional: Road to the Show's Packages from a server on this PC), `stubs.c` (named stubs) | ours (main/boot/cfg adapted from TotK, attributed in headers) |
| `runtime/gfx/` | TotK's graphics stack: NVN front end, Vulkan back end, present and Win32 window, shader client (docs/graphics-plan.md); `gfx_stubs.c` switches off the TotK-only features; `nvn_api_more.c` has NVN procs TotK never implemented | TotK Recomp `ae30a61`; changes marked `fastball` |
| `tools/build.py` | Writes `work/build/modules.c` and `build.ninja`, compiles with LLVM clang, links `work/build/mlb.exe` with lld | ours |
| `tools/data/expected-<version>.json` | per game version (1.0.15, 1.0.20): ExeFS hashes the loader checks, main's image size, RomFS size | ours |
| `tools/addrmap.py` | finds a 1.0.15 address of `main` in 1.0.20 (masked instruction or data match) | ours |

The generic/game split follows AGENTS.md: `runtime/core` holds nothing MLB-specific, and `runtime/mlb` holds nothing
that another title couldn't replace.

### Changes to vendored files (all marked `fastball` in the source)

- `runtime/core/ipc.c`, `ipc.h`: **CMIF domains** (ConvertToDomain, CopyFromCurrentDomain, domain requests and
  close, interfaces returned as object ids). TotK never needed them, because its symbol overrides catch service use
  first. MLB's SDK 19.3.11 converts `lm`, `appletOE` and `pctl` to domains inside `nninitInitializeSdkModule`. Plus
  the `rt_service_handler(name, &stable_name)` hook, so the game layer can add services without editing `ipc.c`'s
  table.
- `ipc.c` sm: **the NPDM's service access list is enforced** (`rt_service_allowed`, read from main.npdm's ACI0 by
  `runtime/mlb/boot.c`). Names outside it get `NotAllowed` (0x1015), as on Horizon; the SDK probes `pctl:s`,
  `pctl:a`, `pctl:r` and falls back to `pctl`. The TIPC reply keeps its move-handle slot (0) on failure, because
  SDK 19.3.11's `nn::sm::GetServiceHandle` reads the handle from word 3 and the result from word 4 without looking
  at the header **[C, disassembly]**. Without the slot, it read a failure as success with handle 0.
- `runtime/core/memory.c`: **MapMemory (svc 0x04) accepts a module's writable data as the source.** SDK 19.3.11's
  system worker thread (`nn::os::RegisterSystemWorkerHandler`, from `nn::oe::SetupGpuErrorHandler`) keeps its
  0x1000-byte stack in sdk's `.bss` and aliases it into the stack region; TotK's version allowed only heap and alias
  sources, so `MapAliasStack` failed 64 times and the SDK aborted. The pages read as no-access while moved.
- `runtime/core/kernel.c`, `host.h`: `kern_event_clear`, so am can clear its message event when the queue empties.
- `memory.c`, `kernel.c`, `svc.c`, `host.h`: **shared memory** (svc 0x13 MapSharedMemory, 0x14 UnmapSharedMemory,
  handle kind `KERN_SHMEM`). A host service owns each block (`mem_shmem_new`). Guest addresses are host addresses
  and the guest regions are plain reservations, so a mapping cannot alias the block's pages: it is committed in the
  stack/heap block or the alias region and filled with a copy, and `mem_shmem_write` updates the block and every
  mapping. That fits blocks the host writes and the guest reads (time now; hid and applet resources later). Guest
  writes stay in their own mapping (`mem_shmem_read` reads the first one back). QueryMemory reports mappings as
  Shared, and heap growth, stack moves and MapPhysicalMemory refuse to overlap them.
- `memory.c` QueryMemory: **host-owned gaps read Inaccessible**, not TotK's Code/RX. SDK 19.3.11's
  `nn::ro::detail::GetExceptionInfo` (the process name for log observers, found from `nnMain`'s address) walks back
  from a code address while the regions read Code with R or X **[C, disassembly]**. The Code gap below `main` made
  `main`'s "module" start at the gap's base, and `FindModuleHeader` read unmapped host memory there.
- `runtime/core/fs_host.c` (new, from TotK `fs_host.c`) and `fs.h`: **nn::fs by symbol override**, the TotK way:
  `main` imports 29 `nn::fs` functions and calls them through its GOT. Save, temporary and cache storage are TotK's
  host directories with the journaled save commit. Changes:
  - the RomFS is a read-only mount kind of the extracted directory instead of TotK's indexed `romfs.bin`
    (`romfs.c` is not vendored), so `OpenDirectory` works on it too;
  - the title's id and save/temporary sizes come from `fs_title` (`runtime/mlb/game.c`, from the NACP);
  - MLB's extra overloads are added: `GetFreeSpaceSize`, `ExtendSaveData`/`GetSaveDataSize` (sizes kept in
    `<profile>/save-size`), `MountCacheStorage(name)`, `DeleteDirectory`, `GetDirectoryEntryCount`,
    `QueryMountRomCacheSize(size_t *, int)`, `EnableGlobalFileDataCache`;
  - the probe checks the RomFS mount against `romfs_list.tsv`;
  - `<profile>/profile.lock` is held while a game runs, and `fs_save_import` replaces the committed save with another
    host's copy (see "Importing a save from Ryujinx"); `save_recover` finishes an import stopped between its two moves.

## Building and running

```bash
.venv/Scripts/python -I recomp/gen.py --exefs work/update/exefs --out work/gen --modules main,sdk --patches tools/data/patches-1.0.20.txt
.venv/Scripts/python -I tools/build.py
work/build/mlb.exe
```

`tools/build.py` takes the game version from the SHA-256 of `work/update/exefs/main` (or `--exefs DIR`) and stops
when `work/gen` was generated from another version's `main`. The dev tree runs 1.0.20; 1.0.15's files are in
`work/update-1.0.15` and `work/romfs-1.0.15` (AGENTS.md, "Game versions").

### CPUs without AVX2 (`--cpu v2`)

The default build targets x86-64-v3 (AVX2, FMA, BMI2: Intel Haswell 2013 / AMD Excavator, Zen and newer). For older
CPUs `tools/build.py --cpu v2` compiles the runtime and the game code for x86-64-v2 (SSE4.2, POPCNT: Intel Core i 1st
generation, AMD FX / Bulldozer and newer), as TotK Recomp and Astral Chain Recomp do (`build_game.py --cpu v2`):

```bash
.venv/Scripts/python -I tools/build.py --cpu v2 --out work/build-v2
.venv/Scripts/python -I tools/build.py --toolchain <llvm-mingw> --make-runtime-lib --cpu v2 --out work/rtlib   # libmlbrt-v2.a
```

- What changes in a v2 build: no AVX, so `runtime/core/fma.c` gives the Switch's fused multiply-adds their single
  rounding in software, and the core switch stub (`kernel.c` `kern_ap_stub`) saves XMM registers with SSE moves.
  Measured on the dev machine (Ryzen 5 5600X, attract match, unlocked, 2026-11-26): about 146 fps against 161 fps for
  v3, so about 10% slower. Only CPUs without AVX2 get it.
- Guard: `-march` does not bind inline assembly, `target(...)` attributes or intrinsics, so a v2 build runs
  `tools/check_isa.py` over the runtime's objects and fails on any instruction above v2 outside a function it lists as
  dispatched by a CPU check (`sha256.c` `blocks_ni`). The 240 generated objects of a v2 build were checked the same way
  once (clean); they are plain C.
- `mlb.exe` checks the CPU before `main` and before any code built for v3 runs: `runtime/mlb/cpucheck.c`, built for
  baseline x86-64 and run as a CRT initializer (`.CRT$XIU`). It also refuses a runtime and game code of different
  levels (`RT_CPU_LEVEL` against `rt_gen_cpu_level` in `modules.c`): the switch stub of a v2 runtime does not save the
  YMM halves v3 game code may hold. `mlb.log` says which level it was built for (`cpu: game built for x86-64-v2, ...`).
- The setup reads the CPU's level with CPUID (`installer/detect.py` `cpu_level`: a probe of `runtime/core/cpulevel.h`, compiled with the
  setup's clang) and links `libmlbrt.a` or `libmlbrt-v2.a`; `--cpu v2` forces the v2 build for tests. The level is
  part of the build step's key, so the same install on another CPU is built again by Repair.
- Super Odyssey Recomp instead builds its runtime for baseline x86-64 for everyone and adds an AVX-only level
  (`-march=sandybridge`) for the game code. Not taken here: the baseline runtime would cost AVX2 players frame rate,
  and the AVX level would need a v2 runtime whose switch stub saves YMM registers when the CPU has them.

### Profile-guided optimisation of the runtime

The runtime (not the game's code) is compiled with `tools/data/runtime.profdata`, counts of which runtime functions and
branches run most in the attract match. `tools/build.py` uses it by default (`--pgo off` leaves it out), and the
release's `libmlbrt.a` is built with it, so players get the faster runtime without a profile of their own. It holds only
runtime function names and counts: the game's code is never instrumented. Functions changed since the training compile
as without a profile; train again after large changes to the replay:

```bash
.venv/Scripts/python -I tools/build.py --pgo gen        # mlb.exe whose runtime counts (slower)
work/build/mlb.exe                                      # MLB_TRACE=none MLB_SAMPLER=off: the attract match, 3 minutes
.venv/Scripts/python -I tools/pgo.py merge              # work/build/pgo/*.profraw -> tools/data/runtime.profdata
.venv/Scripts/python -I tools/build.py                  # the normal build again
```

Train with the diagnostics off (`MLB_TRACE=none MLB_SAMPLER=off MLB_CAPTURE_KEY=0` in the environment), as players
run: otherwise their branches count as the common case.

The game's RomFS must be extracted first (1.0.20: 37.9 GB, 86,235 files; 1.0.15: 37.4 GB, 84,634 files; about 3.5
minutes at 150-190 MiB/s). Extraction is
resumable, and it writes `work/romfs/romfs.complete` only once every listed file is there at its size; `MountRom`
refuses to start without that marker:

```bash
.venv/Scripts/python -I tools/romfs_dump.py --keys "$APPDATA/Ryujinx/system/prod.keys" --titlekeys "$APPDATA/Ryujinx/system/title.keys" --extract-all "gamefiles/<base>.nsp" "gamefiles/<update>.nsp" work/romfs
```

`work/build/mlb.exe --probe-fs` checks the `nn::fs` overrides without running guest code. It covers all 84,634
RomFS entries, 64 sampled files read whole through each `ReadFile` overload and compared with an independent read, a
RomFS listing, refused RomFS writes, and TotK's save-transaction and storage checks in a temporary profile (including
a crash at each step of a Commit). Status: 108 of 108 checks pass.

### Importing a save from Ryujinx

A Switch save is the same files on every host, so a Ryujinx save needs no conversion: MLB lists its save folders
(`CARR<n>/`, `PLYA<n>/`, `SETT<n>/`, each with `SAVEINFO` and `SAVE_000`) and reads them by name, whatever the
account. Verified: a Ryujinx 1.0.15 save, copied unchanged into a profile with another account id, loaded its
settings and its Road to the Show career. Close the game, then:

```bash
work/build/mlb.exe --import-save
```

With no argument (or `ryujinx`) it reads Ryujinx's save index in `%APPDATA%\Ryujinx` (`imkvdb.arc`, which maps the
title and user to a save folder number) and takes `bis\user\save\<number>\0`; with several Ryujinx users it takes the
save written last and names the others. A folder argument can be a Ryujinx data folder (a portable Ryujinx's
`portable`), a Ryujinx save folder `bis\user\save\<number>`, or any folder holding the save's files (yuzu and its
forks: `nand\user\save\0000000000000000\<user>\0100AA7024094000`). `--profile DIR` picks the profile as usual.

The current save moves whole to `<profile>/save-before-import/<UTC time>/`, which is never pruned: to go back, close
the game and put that folder's contents in `<profile>/save/<uid>/`. The import is refused while a game runs on the
profile (`profile.lock`), and it reports on the console only (it does not replace `mlb.log`). Code:
`runtime/core/save_import.c` (finding the source) and `fs_save_import` in `fs_host.c` (copy, flushed; old save aside;
copy in).

Generation takes about 50 s. A clean build takes about 2.5 min with 10 parallel jobs; after that, runtime-only
changes relink in seconds. Settings come from the environment: `MLB_TRACE=svc,host,ipc,fs,sched,all`,
`MLB_EXEFS`/`MLB_ROMFS`/`MLB_PROFILE`, `MLB_LANGUAGE` (default `en-US`), `MLB_TIMEZONE` (`local`, the default, or `utc`), and every TotK `TOTK_<X>` key as `MLB_<X>`. The log goes to stderr and to
`work/build/logs/mlb.log`.

The toolchain is LLVM clang 23 (`x86_64-pc-windows-msvc`) with lld and Ninja from the venv. MSYS2 is not needed.
`clang_rt.builtins` is linked explicitly. Graphics needs two more steps, once each:

```bash
.venv/Scripts/python -I tools/fetch_deps.py
.venv/Scripts/python -I tools/build_xlate.py
```

The first fetches the pinned Vulkan headers, VMA and SDL 3.4.18 (headers and `SDL3.dll`, for controllers) into
`work/deps` and generates `vulkan-1.lib` from the driver's loader. `tools/build.py` copies `SDL3.dll` next to
`mlb.exe`. The second builds the shader translator `work/xlate/totk_xlate.exe` with .NET 10 NativeAOT; it needs VS 2022's
linker. `tools/shader_smoke.py` translates a sample of MLB's FXB shaders offline: 66 of 66 translate.

## Boot progress (1.0.15; 1.0.20 boots the same way)

1. `mem_init` reserves the guest address space; the loader verifies the ExeFS hashes and maps `main`
   (0x7000000000) and `sdk` (0x7004c00000). It applies 122,737 + 33,849 relocations and binds `main`'s 765
   imports. ✔
2. The guest main thread (priority 44, core 0, 1 MiB stack, matching the NPDM) runs `nn::init::Start`; the rtld
   `loader_ready` callback fires. ✔
3. `nninitInitializeSdkModule` → `nn::lm::Initialize` opens **`lm`** and converts it to a domain. ✔ The SDK's
   log packets are decoded into `[lm]` lines.
4. `nn::oe::Initialize` → `ApmInitialize` opens **`apm`**. ✔
5. `nn::oe::InitializeApplet` → `nn::am::InitializeApplicationInterfaces` opens **`appletOE`** (`runtime/mlb/am.c`):
   the application proxy and its sub-interfaces. The SDK reads the applet resource user id, sets focus handling,
   takes the GPU-error event (its system worker thread starts here, see the memory.c change), and receives the
   initial `FocusStateChanged` message. ✔ We present a docked console in focus: operation mode console, performance
   mode boost, 1920x1080. A preselected user (MLB's NACP requires a startup account) comes from
   `<profile>/account.uid` (`runtime/mlb/account.c`).
6. `nn::aoc::detail::InitializeForHipc` opens **`aoc:u`**: no add-on content. ✔
7. `nn::err::InitializeContextWriterApi` opens **`ectx:aw`**. ✔
8. `nn::pctl::detail::ipc::InitializeIpcSessionIfNeeded` probes `pctl:s/a/r` (NotAllowed) and opens **`pctl`**: no
   restrictions. ✔
9. `nntimeStartup` opens **`time:u`** (`runtime/mlb/time.c`), takes the time zone service and the service's shared
   memory, and maps it read-only. ✔ From then on the SDK reads the clocks from that page without IPC. Its layout is
   taken from SDK 19.3.11's `nn::time::detail::SharedMemoryClient` **[C, disassembly]**:
   - local (user) and network clock contexts at 0x38 and 0x80, each a lock-free pair of 0x20-byte
     `SystemClockContext`;
   - the steady clock at 0xD0, a pair of 0x38-byte adjustment points {base, scale, shift, offset, limit, source id}.
     The steady time point is `(now_ns + clamp(((now_ns - base) * scale >> shift) + offset, limit)) / 1e9`.

   We write a steady clock equal to the guest tick (a new source id each run, as after a reboot) and system clocks
   equal to the host's UTC time. The time zone is the host's or UTC (`MLB_TIMEZONE`). `ToCalendarTimeWithMyRule`
   and `ToPosixTimeWithMyRule` are served; explicit time-zone rules (`LoadTimeZoneRule`) are not yet.
10. `nn::init::Start` runs **module constructors**: `main` DT_INIT (1,025 init_array entries), then `sdk` (87). Both
    leave their constructor flag at 2. ✔
11. **`nnMain` runs.** ✔ Its first SDK call was `nn::fs::QueryMountRomCacheSize`, which went to `fsp-srv` until
    the `nn::fs` overrides took it.
12. **fs**: the game mounts the RomFS as `FS_Rom`, temporary storage as `FS_Temp` and cache storage 0 as
    `FS_GameData` (which exists only with `MLB_CACHE_STORAGE`, step 23), reads
    `FS_Rom:/BOOT/CONFIG_MAIN.TXT`, and logs its own start-up lines (`[lm 0] Compiler: ...`). ✔
13. **Graphics bring-up** (docs/graphics-plan.md). The `nv::` and `nn::vi::` overrides take the place of `nvdrv`, and
    `nvnBootstrapLoader` hands out TotK's NVN procs. The game creates its device, records command buffers and presents
    through Vulkan on the RTX 3060 Ti. ✔ Two procs TotK never needed were added:
    `nvnSamplerPoolRegisterSamplerBuilder` and `nvnQueueWaitSync`. **Correction (2026-11-09):** until then `main.c`
    hard-coded the null back end, so the first runs recorded and "presented" without Vulkan; `MLB_GPU` (default
    `vulkan`) now chooses, and the Vulkan path is what runs. MLB binds one program per shader stage, which TotK's back
    end did not support (every draw was skipped as having no vertex stage); see docs/graphics-plan.md.
14. Network setup, offline: **`nifm:u`** (`CreateGeneralService`) and **`bsd:u`** (`RegisterClient`,
    `StartMonitoring`) are served with nothing behind them (`runtime/mlb/services.c`). ✔
15. **`nn::account`** by symbol override (`runtime/mlb/account.c`, TotK's six overrides). The preselected user opens
    with the local Uid, and its nickname comes from `MLB_NICKNAME` (default `Player`). The network service account
    reads as unavailable; its other online calls stop with a named fatal, so no SDK code ever sees a host-made user
    handle. ✔
16. **Input** by symbol override (`runtime/mlb/hid.c`): full-key controllers, one player per connected gamepad
    through SDL3 (Xbox and PlayStation pads: DualSense, DualSense Edge, DualShock 4), and the keyboard as player 1
    (map in the file header; `MLB_PAD_MAP`, `MLB_RUMBLE`). `MLB_PAD_DRIVER=xinput`, or no `SDL3.dll` beside the exe,
    falls back to XInput (Xbox pads only). ✔
    Players (`MLB_PAD_PLAYERS=each`, default; issue #26): the first pad connected is NpadId No1, the next No2, up to
    No4; a pad keeps its number while connected and a new one takes the lowest free number. The game registers No1
    to No4 and No6 to No8 (`main+0x13443d8`, 1.0.20), keeps a pad record per id, reads every id whose style set says
    connected, and for a local match of n players shows the controller applet for up to n (`main+0x1344274`), which
    `ShowControllerSupport` answers with the players connected. Rumble goes to the pad of the handle's player.
    `MLB_PAD_PLAYERS=one` merges every pad into player 1 (buttons ORed, each stick from the pad pushed furthest), the
    port's behaviour before.
    Face buttons: the game works in PlayStation buttons and its Switch build maps them by place (`main+0x1343380`:
    Switch A is circle, B cross, X triangle, Y square), so by position throws, swings and pitches sit where they do
    on PlayStation and Xbox; but its menus confirm with circle (`BUTTON_MASK_UI_ACCEPT` = 0x2000, `main+0x4a36f4`),
    the east button. One icon cell serves both (the menu's "accept" and the throw to first are the Switch A cell), so
    no fixed mapping gets menus and play right. `MLB_PAD_MAP=auto` (default) maps by label in menus and by position
    in play, and the icons follow: the game logs every screen its UI engine enters (`uiEngine: Entering screen:
    [<movie>->...]`), and the last menu movie (`TitleLoadingScreen`, `PM_Main_Alternate`, `Pause_Menu`, ...) or play
    HUD movie (`Scorebar`, `PitchSelect_Bars`, `Fielding_Assets`, ...) entered decides (`hid.c hid_game_log`;
    checked against two recorded sessions with memory snapshots). A face button held across a switch is ignored
    until released. `label` and `position` fix one mapping. F8 (`statedump.c`, diagnostic) writes main's data and
    bss to `<profile>/state-<frame>.bin` for such comparisons.
    The game's button prompts show the controller in use (`runtime/mlb/prompts.c`, `MLB_PROMPTS=auto|nintendo|xbox|
    playstation`, the *Button icons* row of the Controls tab). The game has one set of prompts for all players, so
    `auto` follows player 1's pad (with `MLB_PAD_PLAYERS=one`, the pad used last). MLB draws every prompt button from one 1024x1024 BC7
    texture (the `symbols` entry of `NSW/UI/ART/FONTS/FONTS.TEX`), which has only Switch buttons; the RomFS has no Xbox
    or PlayStation art (`PS4/` and `PS5/` hold animation, stadium and subtitle files). `tools/make_prompts.py` draws
    Xbox and PlayStation icons in the game's style (Bahnschrift letters, drawn symbols) and compresses them to BC7
    (`work/gen/prompt_icons.h`, run by `build.py`); `vk_texture.c` copies them over that texture's image after each
    upload, and uploads it again when the controller kind or the setting changes. `--check <png>` checks the cells
    against the texture exported from a capture and writes previews.
    The play HUD (pitch list, fielding diamond) draws its buttons from other textures: one 128x128 BC7 texture per
    button, entries of `NSW/UI/GAME/GAME.TEX` (and other packs, same bytes) named after the PlayStation buttons with
    the Switch ones drawn in (`button_x` is Switch B, `button_circle` A, `button_triangle` X, `button_square` Y,
    `button_l1/r1/l2/r2` L/R/ZL/ZR; found with a RenderDoc capture of the pitch list). `make_prompts.py` treats each
    as a target with its own hash and patches it the same way; `--check-hud <GAME.TEX>` checks them against the game's
    pack (TX12NSW: 0x140-byte entries from 0x30, LZ4 blocks, block-linear BC7) and writes previews.
17. More offline services: `ssl`, `csrng` (host random bytes), `ngc:u` (no word list, nothing flagged), `friend:u`
    (an empty notification queue whose event never fires), `nim:eca`, `nsd:u` (environment `lp1`, `ResolveEx`), and
    `pctl`'s free-communication commands (permitted). The SDK's own `nn::account` user lists are overridden too
    (`ListOpenUsers` is called from the SDK's friends code). ✔
18. **Audio**: `audren:u` is TotK's renderer and mixer (`runtime/core/audren.c`, `audio_mixer.c`); `audout:u` is
    Astral Chain Recomp's (`runtime/core/audout.c`). MLB uses both: a 6-channel `AudioOut` that is folded to stereo, and
    a renderer with one device sink. Output is WASAPI (`runtime/core/platform/audio_win32.c`) instead of SDL3.
    `audin:u` lists no microphone. ✔ The AudioOut needed two changes from Astral's behaviour: the buffer event is
    signalled again while a played buffer waits, and `GetAudioOutBufferCount` counts only unplayed buffers (MLB waits
    for that count to fall before it takes buffers back).
19. The game opens its intro movies (`UI/MOVIES/INTRO/*.BIK`, Bink, decoded by the game itself) and logs
    `ATT_SCREEN: FMV player init.` It then runs at 60 fps with two draws a frame for at least 75 s. ✘ **Current
    stop:** every presented frame is black (`MLB_SHOT` captures). Under investigation: MLB samples through separate
    texture and sampler handles (`nvnCommandBufferBindSeparateTextures`), and uploads movie frames with
    `nvnTextureWriteTexels`.
20. **Offline packages** (optional, `MLB_OFFLINE_PACKAGES=1`, off by default; `runtime/mlb/packages.c`,
    [tools/packages](../tools/packages/README.md)). Road to the Show hands out and opens its equipment packs through
    the game's online service. With the setting on, the runtime starts a small server on this PC before the modules
    are linked, serves `nn::socket` on host sockets that reach `127.0.0.1` only, answers `nifm`, `friend:u` and the
    network service account as a console that is online, and puts the server's own certificate in place of the ones
    the game trusts, in memory. The game's online code then logs in to that server and nowhere else. `time:u` serves
    `LoadTimeZoneRule`, `ToCalendarTime` and `ToPosixTime`, which the game calls once it is online. With the setting
    off none of it is installed.
21. **Finished tutorials** (`MLB_KEEP_TUTORIALS`, on by default; `runtime/mlb/tutorials.c`). The game keeps the
    tutorials a player has finished only in its online profile, which the server sends back after each login, so
    offline every tutorial came back on every start. The runtime keeps the list in `<profile>/tutorials-seen.txt`
    and gives it back to the game: it wraps the eight function pointers through which the game asks whether a
    tutorial was seen or marks one seen. With the setting off nothing is installed.
22. **Road to the Show hub upgrades saved** (`MLB_RTTS_AUTOSAVE`, on by default; `runtime/mlb/rtts_autosave.c`).
    The game saves a career after a played game and on a few game events, not after hub purchases, so perk and
    attribute upgrades bought in the hub were lost by quitting before the next game. The runtime wraps the eleven
    menu commands that buy or refund upgrades; after one, the game's own after-game autosave (with its own checks)
    runs at the next screen change, normally when the player leaves the Perks, Tools or pitch upgrade screen. While
    a new player is made (from the BpCreation screen until the Road to the Show home opens) the runtime saves
    nothing: making a player runs the same commands, and a save there left a half-made career with the default name
    "Joe Random" when the game closed before its own next save. With the setting off nothing is installed.
23. **Cache storage** (`MLB_CACHE_STORAGE`, on by default; `runtime/mlb/cache_storage.c`). The game mounts cache
    storage 0 as `FS_GameData` and keeps custom batting cameras, Audio Unlockables, an inventory copy and other files
    there, but never creates it and its NACP declares none, so the mount failed and those files were never kept
    (issue 19). The runtime creates `<profile>/cache/0` at start.

Commands each service has not been seen to use stop with a named fatal (`ipc_unimplemented`), so every new one is
found where it is first needed; commands that only pass a setting are accepted and logged under `MLB_TRACE=ipc`.

`tools/disasm.py` disassembles a module range or symbol (with PLT call targets named) for reading SDK code paths
such as the ones above.

`MLB_SHOT=<frame>[,<frame>...]` saves those presented frames as `<profile>/shot-<frame>.png`.

Still ahead: the frames' correctness (step 19), then the menus, and the save path as the game first uses it.

## Approach for services

TotK replaces about 160 `nn::*` functions by symbol (`HOST_OVERRIDE`), which works only for calls through a GOT,
that is, from `main` into `sdk`. MLB's SDK initialises oe/am/apm/lm itself, through direct calls inside `sdk`, so
those services are implemented at IPC level, in `runtime/mlb/services.c` and later one file per service.
Power Moon Recomp's `runtime/services/` (MIT) is the closest reference for that style. Symbol overrides remain the
tool for calls from `main` (fs, hid, audio, NVN).
