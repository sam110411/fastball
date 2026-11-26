// fastball: does this CPU run the instructions mlb.exe was compiled for? Checked before main, before any code built
// with -march=x86-64-v3 runs, so an older CPU gets a message instead of an illegal instruction somewhere in the start.
//
// mlb.exe is built for one level (tools/build.py --cpu): x86-64-v3 (AVX2, the default) or x86-64-v2 (SSE4.2) for
// CPUs without AVX2; the setup picks the level from the CPU (installer/detect.py cpu_level). An install copied to another PC,
// or one a setup without the v2 build (0.2.1 and older) made, can still meet a CPU below it.
//
// Two levels come together in one exe: the runtime's (RT_CPU_LEVEL, compiled into libmlbrt.a or libmlbrt-v2.a) and
// the game code's (rt_gen_cpu_level, written into modules.c with the generated code). They must agree: the core
// switch stub (kernel.c kern_ap_stub) saves full YMM registers only in a v3 runtime, and v3 game code may keep values
// in their upper halves.
//
// tools/build.py compiles this file for baseline x86-64 (-march=x86-64), whatever --cpu says.
#include <stdio.h>
#include <windows.h>

#include "cpulevel.h"
#include "host.h"

#ifndef RT_CPU_LEVEL
#define RT_CPU_LEVEL 3
#endif

const int rt_runtime_cpu_level = RT_CPU_LEVEL;

// As plat_fatal_notice: a message box for the installed (windowed) game, the console for a dev build started there.
static void cpucheck_fail(const char *text) {
    fprintf(stderr, "FATAL: %s\n", text);
    wchar_t w[1024];
    if (!GetConsoleWindow() && MultiByteToWideChar(CP_UTF8, 0, text, -1, w, 1024))
        MessageBoxW(NULL, w, L"Fastball", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    ExitProcess(1);
}

static int cpucheck(void) {
    if (rt_gen_cpu_level != RT_CPU_LEVEL)
        cpucheck_fail("This copy of the game was built from mismatched parts (game code for one CPU level, runtime "
                      "for another). Start Fastball.exe with Shift held and pick Repair.");
    int have = cpulevel();
    if (have >= RT_CPU_LEVEL) return 0;
    if (have >= 2)
        cpucheck_fail("This copy of the game was built for a CPU with AVX2, and this PC's CPU has none. Start "
                      "Fastball.exe with Shift held and pick Repair: it builds the game again for this CPU.");
    cpucheck_fail("This CPU is too old for the game: it needs SSE4.2 and POPCNT (Intel Core i 1st generation, AMD "
                  "FX / Bulldozer or newer).");
    return 1;
}

// A C initializer of the CRT (.CRT$XIU, the users' slot between the CRT's own XIA-XIC and XIZ): it runs after the C
// runtime is ready and before C++ constructors (XC*) and main, in both the MSVC and the mingw-w64 CRT. Not XIB: the
// MSVC CRT sets up stdio in XIC, and an fprintf from XIB crashed. used: nothing references it by name.
__attribute__((section(".CRT$XIU"), used)) static int (*const cpucheck_init)(void) = cpucheck;

void cpucheck_log(void) {
    int have = cpulevel();
    rt_log("cpu: game built for x86-64-v%d, this CPU runs x86-64-v%d%s", RT_CPU_LEVEL, have,
           RT_CPU_LEVEL < have ? " (a faster build fits it: Repair in the setup builds it)" : "");
}
