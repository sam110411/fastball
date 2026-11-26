// The x86-64 instruction set level of this CPU, from CPUID: 3 (x86-64-v3: AVX2, FMA, BMI1/2, F16C, LZCNT, MOVBE, with
// the OS saving the YMM state), 2 (x86-64-v2: SSE3, SSSE3, SSE4.1, SSE4.2, POPCNT, CMPXCHG16B, LAHF/SAHF) or 0 (older).
// The port is built for one of the first two (tools/build.py --cpu): the default v3, or v2 for CPUs without AVX2.
//
// Header only, so the setup's CPU probe (installer/detect.py cpu_level) and the runtime's start check
// (runtime/mlb/cpucheck.c) read the same bits. Compile the caller for baseline x86-64: code built with -march=x86-64-v3 may use AVX or BMI
// instructions for these very tests and stop with an illegal instruction on the CPUs they are meant to catch.
// Not __builtin_cpu_supports("x86-64-v3"): that reads the C runtime's CPU model, which older runtimes fill without the
// level bits (TotK Recomp installer/hostcheck.c, where these tests come from).
#pragma once
#include <cpuid.h>

static inline int cpulevel_v2(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    const unsigned sse3 = 1u << 0, ssse3 = 1u << 9, cx16 = 1u << 13, sse41 = 1u << 19, sse42 = 1u << 20,
                   popcnt = 1u << 23;
    const unsigned need = sse3 | ssse3 | cx16 | sse41 | sse42 | popcnt;
    if ((c & need) != need) return 0;
    if (!__get_cpuid(0x80000001, &a, &b, &c, &d)) return 0;
    return (c & (1u << 0)) != 0;   // LAHF/SAHF in 64-bit mode
}

static inline int cpulevel_v3(void) {
    if (!cpulevel_v2()) return 0;
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    const unsigned fma = 1u << 12, movbe = 1u << 22, osxsave = 1u << 27, avx = 1u << 28, f16c = 1u << 29;
    const unsigned need = fma | movbe | osxsave | avx | f16c;
    if ((c & need) != need) return 0;
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    if ((lo & 6) != 6) return 0;   // XMM and YMM state enabled by the OS
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d)) return 0;
    const unsigned bmi1 = 1u << 3, avx2 = 1u << 5, bmi2 = 1u << 8;
    if ((b & (bmi1 | avx2 | bmi2)) != (bmi1 | avx2 | bmi2)) return 0;
    if (!__get_cpuid(0x80000001, &a, &b, &c, &d)) return 0;
    return (c & (1u << 5)) != 0;   // LZCNT (ABM)
}

static inline int cpulevel(void) { return cpulevel_v3() ? 3 : cpulevel_v2() ? 2 : 0; }
