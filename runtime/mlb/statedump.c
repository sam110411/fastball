// fastball (diagnostic): F8 in the game window writes main's writable memory (data and bss) to
// <profile>/state-<frame>.bin, with a beep. Snapshots taken in different game situations (a menu, live fielding) are
// compared offline to find the game's own variables that tell them apart (the controller face-button mode, hid.c).
// Only with MLB_STATEDUMP=1: a player's report (0.1.3, RTTS batting) showed F8 pressed three times by mistake, each a
// beep, a 42 MB file in their profile folder and an 88 ms frame. Off, it costs one int test per presented frame.
#include <stdint.h>
#include <stdio.h>

#include "cfg.h"
#include "host.h"
#include "keys.h"
#include "platform/platform.h"

void mlb_state_dump_tick(uint64_t frame) {
    static int on = -1, was;
    if (on < 0) on = cfg_int("STATEDUMP") != 0;
    if (!on) return;
    int down = wsys_key_held(KEY_F8);
    if (!down || was) {
        was = down;
        return;
    }
    was = 1;
    const RtLoaded *m = ldr_count() ? ldr_module(0) : NULL;
    if (!m) return;
    char path[1100];
    snprintf(path, sizeof path, "%s/state-%llu.bin", rt_config()->profile, (unsigned long long)frame);
    FILE *f = fopen(path, "wb");
    if (!f) {
        rt_log("statedump: cannot write %s", path);
        return;
    }
    uint64_t lo = m->base + m->data_off, hi = m->base + m->size, written = 0;
    static const uint8_t zero[4096];
    for (uint64_t a = lo; a < hi; a += 4096) {   // a page that cannot be read is written as zeros
        const void *p = mem_guest_readable(a, 4096) ? (const void *)(uintptr_t)a : zero;
        written += fwrite(p, 1, 4096, f);
    }
    fclose(f);
    plat_beep();
    rt_log("statedump: frame %llu: main+%#llx..+%#llx (%llu KiB) to %s", (unsigned long long)frame,
           (unsigned long long)m->data_off, (unsigned long long)m->size, (unsigned long long)(written >> 10), path);
}
