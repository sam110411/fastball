# Linux and Steam Deck: plan and status

Written 2026-11-19. The question was: what is the best way to bring Fastball to Linux and the Steam Deck?

## Short answer

Build a **native Linux `mlb`**, the way TotK Recomp (the project our runtime came from) already does, rather than
running the Windows build under Proton. Most of the work exists upstream at the very commit we forked from
(`ae30a61`): we left its Linux files out at the start, and the parts we wrote ourselves since then are small and
mostly sit behind interfaces that already have a Linux side.

## What the reference projects do

| Project | Linux | How |
|---|---|---|
| TotK Recomp `ae30a61` | Plays at 60 fps on RADV and NVIDIA; Steam Deck not verified at that commit | `platform/linux.c` behind the same `platform.h` as ours; window, input, menu and keyboard on SDL3 (`*_sdl.c` in place of `*_win.c`); `build_game.py` builds with the system clang/lld and links Vulkan and SDL3; Linux release (`make_release_linux.py`) bundles clang and lld so the player's PC builds the game code, as on Windows; a `DEVICE_PROFILE` with Steam Deck defaults |
| Super Odyssey Recomp | Experimental, plays on Steam Deck | AppImage; builds the game code on the player's PC (20 to 25 minutes on a Deck); "Add to Steam" button writes a non-Steam shortcut for Gaming Mode |
| Power Moon Recomp | Playable | AppImage built in Ubuntu 22.04 containers (glibc 2.35+) |
| Astral Chain Recomp | Same as TotK (same `linux.c`, `*_sdl.c`) | |

What they learned that applies to us (TotK `docs/building.md`, "Linux"):

- **Kernel 6.7 or later.** Windows tells us which pages of guest memory the CPU wrote (`GetWriteWatch`); Linux has no
  such call, and `linux.c` uses userfaultfd write protection with `PAGEMAP_SCAN` (new in 6.7). SteamOS 3.8 qualifies.
- **Shadow pool storage.** On Linux the GPU does not import guest memory directly: each CPU-visible pool gets a copy
  that is synced before each submit (about 0.2 ms a frame and about 1 GB more memory in TotK). Our `vk_memory.c`
  already has this path: it is what we use under RenderDoc.
- **The Deck's GPU has no ASTC**: textures go through the compute decoder (`vk_astc.c`), already there.
- **The shader translator** builds for `linux-x64` with the same .NET command (`build_xlate.py`).

## Why not Proton

Proton could run the Windows `mlb.exe` (Wine implements `GetWriteWatch`, fibers and Vulkan), and it is worth a try as
a stopgap. But the player still has to build `mlb.exe`, which means running clang and the setup under Wine, and every
frame pays Wine's write-watch emulation (a signal per first write to a page) on top of the translation layer. The
native build avoids both, and all the reference projects chose it.

## What has to change in Fastball

Already portable (from TotK, unchanged or nearly): the CPU and kernel (`kernel.c` has both the Windows and the ELF
version of its assembly stub), memory, loader, IPC, the Vulkan back end, `hostfn.c`'s override tables, audio mixing.

| Windows-only piece | Linux replacement | Size |
|---|---|---|
| `platform/win32.c` | TotK's `platform/linux.c`, plus our four additions (`plat_fatal_notice`, `plat_console_attach`, `plat_message`, `plat_fs_lock`) | copy + ~60 lines |
| `platform/audio_win32.c` (WASAPI) | `platform/audio_sdl.c`: an SDL3 audio stream behind the same `audio_out.h` | ~150 lines |
| `platform/prof_win.c` (sampler) | none: `perf.c`'s weak versions (no sampler on Linux at first) | 0 |
| `gfx_window_win.c` | TotK's `gfx_window_sdl.c`, its keyboard and mouse hooks pointed at ours | copy + adapt |
| `ui/menu_win.cpp` | TotK's `ui/menu_sdl.cpp` | copy + adapt |
| `swkbd_win.c` | TotK's `swkbd_sdl.c` | copy |
| `hid.c`: keyboard by `GetAsyncKeyState`, SDL3.dll loaded by hand, XInput fallback | keyboard from the window system (`wsys_key_down`), `libSDL3.so.0` | small |
| F12 / F9 / F8 keys (`gfx_stubs.c`, `statedump.c`), `MessageBeep`, memory status | `wsys_key_down`, platform calls | small |
| `packages.c` (Winsock) | BSD sockets (the offline packages server; can stay off at first) | medium |
| `cfg.c` (`MoveFileExW`) | `plat_file_replace` | tiny |
| DLSS (`vk_dlaa.c`), FSR 3.1/4 (`vk_ffx.c`) | unavailable on Linux at first (the settings menu greys them out with the reason); FSR 1 and FXAA stay | 0 |
| `tools/build.py` (MSVC or llvm-mingw) | a Linux target: system clang + lld, `-lvulkan -lSDL3 -lm -lpthread -ldl`, `$ORIGIN` rpath, no `.rc` | medium |
| `tools/build_xlate.py` | `-r linux-x64` | tiny |
| Setup and launcher (`installer/`) | TotK's installer already runs on Linux (glibc and kernel checks); release as a tarball or AppImage with clang and lld bundled | larger, last |

## Steam Deck specifics

- SteamOS 3.8 or later (kernel 6.7+). Desktop distributions: any with kernel 6.7+, glibc 2.35+ and a Vulkan 1.3 driver.
- Zen 2 has AVX2, so the `-march=x86-64-v3` build runs on it.
- Controls: Steam Input presents the Deck's controls as a gamepad that SDL3 reads; nothing Deck-specific in `hid.c`.
- Screen 1280x800 (16:10): a Deck profile like TotK's `DEVICE_PROFILE` (borderless, sensible resolution scale).
- Memory: 16 GB shared between CPU and GPU, minus the shadow pools' extra ~1 GB. To be measured.
- Speed: in a match our frame rate is held by the game's own CPU code (main thread and three job workers). The Deck's
  Zen 2 at 3.5 GHz is well behind a Ryzen 5 5600X there, so 60 fps in matches is not a given. To be measured; the
  Deck's frame limiter and a lower resolution scale are the levers.
- Gaming Mode: a non-Steam shortcut (Super Odyssey's "Add to Steam" is the model), launched under gamescope.
- Building the game code on the Deck takes a while (Super Odyssey: 20 to 25 minutes; our generated C is about 640 MB).

## Phases

1. **Runtime builds on Linux** (this branch, `linux`): the table above minus the setup, `tools/build.py --linux`.
   Checked by compiling in WSL (Ubuntu 26.04); WSL's kernel (6.6) is too old for the write watch, so real play needs a
   Linux PC or a Deck.
2. **First boot on real Linux**: the user (or a tester) runs it on a Linux PC or a Deck; fix what breaks.
3. **Deck profile and measurements**: defaults, frame rate, memory, battery.
4. **Release**: the setup on Linux, bundled toolchain, "Add to Steam", release notes.

## Building on Linux (phase 1)

Needs clang, lld, ninja, the SDL3 and Vulkan libraries (`libsdl3-dev`, `libvulkan-dev` on Debian and Ubuntu), Python 3,
and `work/deps` from `tools/fetch_deps.py` (its Vulkan, VMA and SDL3 headers are the same on every OS; the Windows
`.lib` and `.dll` there are not used). Then, from the repository root:

```
python3 -I tools/build.py --out work/build-linux        # work/build-linux/mlb
```

`tools/build.py` picks the files of the OS it runs on: `*_win.*`, `platform/win32.c` and `audio_win32.c` on Windows,
`*_sdl.*`, `platform/linux.c` and `audio_sdl.c` on Linux. The executable links `libvulkan.so.1` and `libSDL3.so.0` (one
beside `mlb` comes first: `$ORIGIN` rpath), with libstdc++ static.

The shader translator: `tools/build_xlate.py` still builds the Windows one only. For Linux it needs `-r linux-x64` and
no vswhere check (NativeAOT links it with clang), as TotK's script does. That change is held back on purpose: the
translator's cache identity hashes `build_xlate.py` itself, so any edit to it makes every player's shader cache rebuild
once at the next release. It goes in together with the next real translator change.

What changed in the runtime for Linux (this branch):

- `runtime/core/platform/linux.c`: TotK's, plus `plat_fs_lock` (an `flock`), `plat_fatal_notice` and `plat_message`
  (an SDL message box when there is no terminal), `plat_console_attach`, `plat_beep`, `plat_renderdoc_loaded`.
- `runtime/core/platform/audio_sdl.c`: the `audio_out.h` output on an SDL3 audio stream (PipeWire, PulseAudio, ALSA).
- `runtime/gfx/gfx_window_sdl.c`, `runtime/gfx/ui/menu_sdl.cpp`, `runtime/mlb/swkbd_sdl.c`: TotK's, with our menu keys
  (F1 only, F10) and without TotK's mouse camera. The text entry takes the game's starting text for now.
- `runtime/gfx/keys.h`: one way to poll the keyboard (hid.c's controls, F8, F9, F12) on both window systems, in place
  of `GetAsyncKeyState` in four files.
- `hid.c`: SDL3 linked on Linux, no XInput. `packages.c`: BSD sockets (Linux's socket option numbers, `MSG_NOSIGNAL`).
  `cfg.c`, `time.c`, `fs_host.c`, `vk_ffx.c`, `vk_device.c`, `statedump.c`, `rdoc.c`, `gfx_stubs.c`: the MSVC and
  Win32 calls behind `#ifdef _WIN32` or platform calls.

Not on Linux yet: RenderDoc's in-application API (`rdoc.c`: `librenderdoc.so`), the sampling profiler
(`prof_win.c`), FSR 3.1 / 4 (AMD's runtime is Windows only), DLSS (NGX on Linux needs `libnvidia-ngx-dlss.so`, not
looked at), the text entry window, the offline packages server's bundled Python.

## Status

Phase 1 is done (2026-11-19). The whole game (runtime plus generated code) builds and links for Linux, and the
Windows build (MSVC and llvm-mingw) is unchanged in behaviour: it boots, plays sound and reads the controller as before.

First boot, in WSL2 (Ubuntu 26.04, kernel 6.6) on Mesa's software Vulkan (lavapipe), since that PC has no Linux GPU:

- The SDL window opens (X11 through WSLg), Vulkan comes up, the shaders translate with the Linux translator, sound
  plays through SDL and PulseAudio, the controller layer starts, and the game runs its intro movies (frame 3600 in
  150 s). No crash.
- The intro movie and the studio logo show a green dotted overlay that the Windows build does not have, also with
  `TOTK_SHADOW=1` (the shadow pools Linux uses). Two things in that test differ from a real Linux PC: lavapipe, and
  WSL's 6.6 kernel, which has no write watch, so every pool is copied whole before each submit ("untracked"). It needs
  a real GPU and kernel 6.7+ to say whether it is ours. First thing to check in phase 2.
- `kernel: ... asynchronous preemption ... N failed` counts above zero on Linux (Windows: 0). Not harmful in the test;
  worth a look on real hardware.

How that test was run, for repeating it: without root, `apt-get download` and `dpkg -x` unpack clang, lld, ninja, SDL3
and Vulkan into a folder in the WSL home; `LD_LIBRARY_PATH` and `PATH` point there; `VK_ICD_FILENAMES` picks
`lvp_icd.json`; `MLB_PROFILE` points at a scratch folder so the dev saves are never touched. The .NET SDK goes into
`~/.dotnet` with Microsoft's `dotnet-install.sh`.

Next (phase 2): a run on a real Linux PC or a Steam Deck (SteamOS 3.8+), to check the overlay above, frame rate,
memory, and the controller under Steam Input.
