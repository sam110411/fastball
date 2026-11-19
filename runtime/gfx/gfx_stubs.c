// TotK Recomp graphics features the MLB port does not carry (docs/graphics-plan.md, "Not ported"): their entry
// points, as the vendored back end calls them, each switched off. Declarations in vk.h, vk_dlaa.h, gfx.h, flight.h,
// input.h, ui/menu.h and host.h (all from TotK Recomp @ ae30a61). A feature that comes back replaces its stubs here.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <string.h>

#include "cfg.h"
#include "flight.h"
#include "gfx.h"
#include "host.h"
#include "input.h"
#include "keys.h"
#include "platform/platform.h"
#include "ui/menu.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_exec.h"

// ---- frame_rate.c: MLB's own is runtime/mlb/framerate.c (MLB_FPS_LIMIT through the game's frame timer, no patches)

// ---- resolution.c: TotK's output/scene size patches and FOV
void res_fov_live(void) {}
void res_present(Cpu *c) { (void)c; }

// ---- ov_movie.c: TotK's VP9 movie decoder (MLB plays Bink; not ported yet)
void movie_tick(Cpu *c) { (void)c; }

// ---- perf.c: runtime/core/perf.c

// ---- flight.c, rdoc.c, vk_crumbs.c, vk_dump.c, pass marks: diagnostics
int flight_on;
void vk_crumb(VkCommandBuffer cb, int kind, uint64_t n) { (void)cb, (void)kind, (void)n; }
void vk_crumbs_init(VkCtx *c) { (void)c; }
void vk_crumbs_release(void) {}
void vk_crumbs_report(void) {}
int vk_dump_armed;
void vk_dump_init(void) {}
void vk_dump_copy(VkExec *ex, uint32_t op, const void *payload) { (void)ex, (void)op, (void)payload; }
void vk_dump_dispatch(VkExec *ex, const GfxShader *cs, VkPipeline p, const VkPipeInfo *info, const p_dispatch *d) {
    (void)ex, (void)cs, (void)p, (void)info, (void)d;
}
void vk_dump_draw(VkExec *ex, const VkPipeState *s, VkPipeline p, const VkPipeInfo *info) {
    (void)ex, (void)s, (void)p, (void)info;
}
void vk_dump_pass_end(VkExec *ex) { (void)ex; }
void vk_dump_present(uint64_t frame) { (void)frame; }
// vk_pass_mark_begin / vk_pass_mark_end: vk_passtime.c (GPU time per pass and dispatch, MLB_TRACE=perf)
void vk_pass_mark_cmd(VkCommandBuffer cb, uint32_t id, int end) { (void)cb, (void)id, (void)end; }
void vk_pass_marks_report(void) {}
int vk_pass_marks_want_nv(void) { return 0; }
// fastball, our own: an RGBA8 PNG with stored (uncompressed) deflate blocks. Big files, no zlib needed; for
// test shots only.
static uint32_t png_crc(uint32_t c, const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            t[i] = v;
        }
    c = ~c;
    while (n--) c = t[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n) {
    uint8_t be[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    fwrite(be, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t c = png_crc(png_crc(0, (const uint8_t *)type, 4), d, n);
    uint8_t cb[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
    fwrite(cb, 1, 4, f);
}

void rt_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    size_t row = (size_t)w * 4 + 1, raw_n = row * h, blocks = (raw_n + 65534) / 65535;
    size_t z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t *raw = malloc(raw_n), *z = malloc(z_n);
    FILE *f = fopen(path, "wb");
    if (!raw || !z || !f || z_n > 0xFFFFFFFFu) {
        rt_log("gfx: screenshot %s not written", path);
        free(raw), free(z);
        if (f) fclose(f);
        return;
    }
    for (uint32_t y = 0; y < h; y++) {
        raw[y * row] = 0;   // filter: none
        memcpy(raw + y * row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }
    size_t o = 0;
    z[o++] = 0x78, z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i += 65535) {
        size_t n = raw_n - i < 65535 ? raw_n - i : 65535;
        z[o++] = i + n == raw_n;
        z[o++] = (uint8_t)n, z[o++] = (uint8_t)(n >> 8), z[o++] = (uint8_t)~n, z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + i, n);
        o += n;
        for (size_t k = 0; k < n; k++) a = (a + raw[i + k]) % 65521, b = (b + a) % 65521;
    }
    uint32_t ad = b << 16 | a;
    z[o++] = (uint8_t)(ad >> 24), z[o++] = (uint8_t)(ad >> 16), z[o++] = (uint8_t)(ad >> 8), z[o++] = (uint8_t)ad;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0};
    fwrite(sig, 1, 8, f);
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)o);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw), free(z);
}
int probe_census_dir(char *dir, size_t n) { (void)dir, (void)n; return 0; }

// ---- per-object motion vectors (vk_mvprev.c, vk_mvsurvey.c); DLSS itself is vk_dlaa.c (camera motion only)
int vk_mvprev_on(void) { return 0; }
void vk_mvprev_draw(VkExec *ex) { (void)ex; }
void vk_mvprev_frame(void) {}
void vk_mvprev_want(uint32_t index_mask) { (void)index_mask; }
int vk_mvprev_find(int index, uint64_t addr, uint64_t size, uint64_t *prev, int *how) {
    (void)index, (void)addr, (void)size, (void)prev, (void)how;
    return 0;
}
uint64_t vk_mvprev_step(int stage, int index, uint64_t size) { (void)stage, (void)index, (void)size; return 0; }
void vk_mvsurvey_draw(VkExec *ex) { (void)ex; }
void vk_mvsurvey_frame(void) {}

// ---- contact shadows (vk_contact.c)
int vk_contact_strength(void) { return 0; }
void vk_contact_pass_begin(VkExec *ex) { (void)ex; }
void vk_contact_flush(VkExec *ex) { (void)ex; }
void vk_contact_release(void) {}

// ---- HDR output (vk_hdr.c): SDR only
int vk_hdr_track;
int vk_hdr_wanted(void) { return 0; }
void vk_hdr_support(float out[4]) { memset(out, 0, 4 * sizeof *out); }
int vk_hdr_surface_format(const VkSurfaceFormatKHR *f, uint32_t n, VkSurfaceFormatKHR *out) {
    (void)f, (void)n, (void)out;
    return 0;
}
void vk_hdr_set_active(int on) { (void)on; }
void vk_hdr_release(void) {}
void vk_hdr_metadata(VkSwapchainKHR sc, int new_sc) { (void)sc, (void)new_sc; }
void vk_hdr_shader_swap(uint32_t **spirv, uint64_t *words) { (void)spirv, (void)words; }
void vk_hdr_draw(VkExec *ex, const VkPipeInfo *info) { (void)ex, (void)info; }
void vk_hdr_image_gone(VkImage image) { (void)image; }
void vk_hdr_present_source(const VkTexImage *win, VkImage *image, VkFormat *format) { (void)win, (void)image, (void)format; }
VkImage vk_hdr_encode(VkCommandBuffer cb, unsigned slot, VkImage src, VkFormat fmt, const int crop[4]) {
    (void)cb, (void)slot, (void)fmt, (void)crop;
    return src;
}
VkImageView vk_hdr_ui_target(VkCommandBuffer cb, uint32_t w, uint32_t h) { (void)cb, (void)w, (void)h; return VK_NULL_HANDLE; }
void vk_hdr_ui_compose(VkCommandBuffer cb, VkImageView dst_view, VkFormat fmt, uint32_t w, uint32_t h) {
    (void)cb, (void)dst_view, (void)fmt, (void)w, (void)h;
}
void vk_hdr_to_srgb8(const uint8_t *in, uint8_t *out, size_t px, int a2r) { (void)a2r; memcpy(out, in, px * 4); }

// ---- keyboard/mouse input (input_win.c): not ported; the in-game settings menu is ui/menu.cpp.
// fastball: MLB_SHOT=<frame>[,<frame>...] saves the presented picture of those frames as
// <profile>/shot-<frame>.png through gfx_present.c's capture path (the one TotK's menu test shots use).
void vk_drawlog_arm_at(long frame);   // vk_exec_draw.c
// One frame's capture, `ahead` presents from now: its draws in the log (MLB_DRAWLOG format) and its picture as
// <profile>/shot-<frame>.png. F12 with MLB_CAPTURE_KEY=1 (F9 under RenderDoc) in the game window asks for one; so does MLB_DRAWLOG=game, 10 s after the game logs
// ===GAME ENTERED=== (services.c).
static volatile long capture_at = -1;
void gfx_capture_arm(long ahead) {
    long f = (long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + ahead;
    vk_drawlog_arm_at(f);
    __atomic_store_n(&capture_at, f, __ATOMIC_RELAXED);
    rt_log("capture: frame %ld", f);
}
// Free commit (RAM plus page file) and RAM, GiB, for the RenderDoc notes below; 0 where unknown.
static void commit_free(double *free, double *total, double *ram) {
    *free = *total = *ram = 0;
#ifdef _WIN32
    MEMORYSTATUSEX ms = {.dwLength = sizeof ms};
    if (!GlobalMemoryStatusEx(&ms)) return;
    *free = (double)ms.ullAvailPageFile / 1073741824.0, *total = (double)ms.ullTotalPageFile / 1073741824.0;
    *ram = (double)ms.ullAvailPhys / 1073741824.0;
#endif
}

// Called by ui/menu.cpp menu_overlay_frame at every present: sets *shot when this frame's picture is to be saved.
int mlb_capture_tick(uint64_t frame, const char **shot) {
    static char path[1100];
    static int f12_was;
    // Under RenderDoc F12 (and Print Screen) is its frame capture: ours moves to F9, so one key takes one capture.
    // fastball: without RenderDoc, F12 only with MLB_CAPTURE_KEY=1. Players press F12 for a screenshot (Steam's
    // key), and the capture writes every render target as a PNG and the frame's draws into mlb.log: a long stall.
    static int key;
    if (!key) {
        key = plat_renderdoc_loaded() ? KEY_F9 : cfg_int("CAPTURE_KEY") ? KEY_F12 : -1;
        if (key == KEY_F9) {
            // A RenderDoc capture copies every dirty resource and keeps two copies of each mapped range in system
            // memory: several GiB of commit on top of the game's. Short of it, RenderDoc's allocations fail and it
            // crashes in the capture (seen with a 1 GiB page file: "Allocation for 89128960 bytes failed").
            double fr, tot, ram;
            commit_free(&fr, &tot, &ram);
            rt_log("capture: RenderDoc is loaded: F12 is its capture, ours (draw log, render targets) is F9; commit free "
                   "%.1f of %.1f GiB%s", fr, tot,
                   tot > 0 && fr < 12 ? " (low: a capture may run out of memory and crash; raise the page file)" : "");
        }
    }
    int f12 = key > 0 && wsys_key_tapped(key);   // nothing is asked of the window system without a capture key
    if (key == KEY_F9) {   // RenderDoc's F12: the free commit at the press (a capture short of it crashes in renderdoc.dll)
        static int rd_was;
        int rd = wsys_key_tapped(KEY_F12);
        if (rd && !rd_was) {
            double fr, tot, ram;
            commit_free(&fr, &tot, &ram);
            rt_log("capture: RenderDoc capture at frame %llu: commit free %.1f of %.1f GiB, RAM free %.1f GiB",
                   (unsigned long long)frame, fr, tot, ram);
        }
        rd_was = rd != 0;
    }
    if (f12 && !f12_was) gfx_capture_arm(2);
    f12_was = f12 != 0;
    void mlb_state_dump_tick(uint64_t frame);   // fastball: F8, statedump.c (diagnostic)
    mlb_state_dump_tick(frame);
    if ((long)frame == __atomic_load_n(&capture_at, __ATOMIC_RELAXED)) {
        snprintf(path, sizeof path, "%s/shot-%llu.png", rt_config()->profile, (unsigned long long)frame);
        *shot = path;
        plat_beep();   // the capture is being taken: a beep tells the player (asynchronous)
    }
    const char *list = cfg_get("MLB_SHOT");
    for (const char *q = list; q && *q;) {
        char *end;
        unsigned long long f = strtoull(q, &end, 10);
        if (end == q) break;
        if (f == frame) {
            snprintf(path, sizeof path, "%s/shot-%llu.png", rt_config()->profile, f);
            *shot = path;
            break;
        }
        q = *end == ',' ? end + 1 : end;
    }
    return 0;
}
#ifdef _WIN32
int input_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res) { (void)h, (void)m, (void)w, (void)l, (void)res; return 0; }
#endif
void input_cursor_suspend(int on) { (void)on; }

// ---- boot.c --frames: every present lands here; log the first and then every 600th
void rt_frame_presented(const tex_t *t, uint64_t n) {
    (void)t;
    if (n == 1 || n % 600 == 0) rt_log("gfx: frame %llu presented", (unsigned long long)n);
}
