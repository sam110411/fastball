// Settings for the runtime: the environment, then values set at run time (cfg_set), then the table's default.
// Interface and defaults from TotK Recomp runtime/cfg.h and cfg.c @ ae30a61 (MIT, LICENSE.totk-recomp); the settings
// file (totk.ini) and the menu's save are not ported. Keys are TotK's names: "TOTK_X" and "X" mean the same setting;
// fastball: MLB_X in the environment wins over TOTK_X, and the table has MLB's own rows (LANGUAGE, TIMEZONE).
#include "cfg.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // MoveFileExW (cfg_save)
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "host.h"
#include "platform/platform.h"

#define LIVE 1
#define RESTART 0

static const CfgMeta metas[] = {
    // fastball rows
    {"LANGUAGE", "en-US", NULL, RESTART},   // am.c GetDesiredLanguage; the NACP lists American English only
    {"TIMEZONE", "local", "local|utc", RESTART},   // time.c: the host time zone, or UTC
    {"SPIN", "1", "0|1", RESTART},   // kernel.c spin-wait gates (TotK reads it without a row)
    {"NICKNAME", "Player", NULL, RESTART},   // account.c GetNickname (up to 32 bytes of UTF-8)
    {"OFFLINE_PACKAGES", "0", "0|1", RESTART},   // packages.c: Road to the Show's Packages from a server on this PC
    {"PAD_MAP", "auto", "auto|label|position", LIVE},   // hid.c: face buttons by label in menus, by place in play
    {"RUMBLE", "1", "0|1", LIVE},   // hid.c: vibration on the controllers
    {"PAD_DRIVER", "sdl", "sdl|xinput", RESTART},   // hid.c: SDL3 (Xbox and PlayStation pads) or XInput (Xbox only)
    {"PROMPTS", "auto", "auto|nintendo|xbox|playstation", LIVE},   // prompts.c: whose buttons the game's prompts show
    {"GPU", "vulkan", "vulkan|null", RESTART},   // main.c: the graphics back end (gfx_null.c chooses by it)
    {"AA", "fxaa", "off|fxaa", LIVE},   // vk_aa.c: antialiasing of the 3D scene before the game lays its HUD over it
    {"AA_STRENGTH", "75", "0..100", LIVE},   // vk_aa.c: how much FXAA softens thin, sub-pixel detail
    // TotK Recomp's rows (cfg.c @ ae30a61); the TotK-only ones are read by nothing here
    // Screen
    {"FULLSCREEN", "0", "0|1", LIVE}, {"WINDOW_SIZE", "1920x1080", NULL, LIVE},
    {"VK_PRESENT", "mailbox", "mailbox|fifo|immediate", LIVE}, {"GPU_INDEX", "", NULL, RESTART},
    {"PERF_OVERLAY", "off", "off|fps|full", LIVE},
    {"OUTPUT_RES", "1920x1080", NULL, RESTART}, {"RENDER_SCALE", "game", "game|100|125|150", RESTART},   // fastball: resolution.c (200 ran out of a game heap)
    // fastball: UPSCALER bilinear by default (the game's own look); fsr: vk_fsr.c, on the 3D scene and the present
    {"UPSCALER", "bilinear", "fsr|bilinear", LIVE}, {"SHARPNESS", "80", "0..100", LIVE},
    // fastball: one DLSS row, off or the mode (vk_dlaa.c; the scene's size follows the mode, resolution.c), read at
    // start; TotK's DLAA switch and its characters' mask (TotK's G-buffer classes) are not used. The preset is live.
    {"DLSS", "off", "off|dlaa|quality|balanced|performance|ultra_performance", RESTART},
    {"DLSS_PRESET", "default", "default|k|j|m|l|e", LIVE}, {"FXAA", "1", "0|1", LIVE},
    // fastball: AMD FSR 3.1 / 4 (vk_ffx.c), off by default like DLSS: the mode, read at start (the scene's size follows
    // it, resolution.c); the version (4: FSR 4 where the GPU runs it, else 3.1) and FSR's own sharpening are live
    {"FSR", "off", "off|native|quality|balanced|performance|ultra_performance", RESTART},
    {"FSR_VERSION", "4", "4|3.1", LIVE}, {"FSR_SHARPNESS", "50", "0..100", LIVE},
    {"ASPECT", "fit", "fit|stretch", LIVE}, {"MOVIE_ASPECT", "keep", "keep|stretch", LIVE}, {"FOV", "100", "70..140", LIVE},
    {"ANISO", "game", "game|2|4|8|16", RESTART},   // vk_texture.c sampler_features
    // colour adjustment (vk_color.c); the preset is the menu's, the four values are what the present uses
    {"COLOR_PRESET", "default", "default|vivid|clarity|custom", LIVE}, {"COLOR_BRIGHTNESS", "0", "-30..30", LIVE},
    {"COLOR_CONTRAST", "100", "70..150", LIVE}, {"COLOR_SATURATION", "100", "50..200", LIVE},
    {"COLOR_GAMMA", "1", "0.8..1.2", LIVE},
    // HDR output (vk_hdr.c): on at start (the tonemap shader is swapped as shaders load); the nits apply at once
    {"HDR", "0", "0|1", RESTART}, {"HDR_PEAK_NITS", "1000", "400..10000", LIVE}, {"HDR_GAME_NITS", "203", "80..500", LIVE},
    {"HDR_UI_NITS", "203", "80..500", LIVE}, {"HDR_HUE", "80", "0..100", LIVE},
    {"DEVICE_PROFILE", "auto", "auto|desktop|deck", RESTART},
    {"SHADOW_RES", "game", "game|256|512|768|1024|1536|2048", RESTART},
    {"SHADOW_SCALE", "1", "1..4", RESTART},
    {"SHADOW_RANGE", "100", "50..400", RESTART}, {"CONTACT_SHADOWS", "0", "0..100", RESTART},
    {"PCSS", "0", "0..200", RESTART},
    // Game
    // fastball: FRAME_QUEUE on (uneven frames at 60): the frame queue (nvn_api_obj.c nvnWindowAcquireTexture).
    // Without it a window image is free only one period after its present, so a frame that runs a little long misses
    // its slot; with it the game draws the next frame while the previous one waits for its turn.
    // fastball: FPS_LIMIT 60 the game's own; any other rate through the game's frame timer (framerate.c)
    {"FPS_LIMIT", "60", "30|40|45|60|90|120|144|165|240|display", LIVE}, {"FPS60", "1", "0|1", LIVE}, {"FRAME_QUEUE", "1", "0|1", LIVE},
    {"REALTIME", "1", "0|1", RESTART}, {"CLOTH_REALTIME", "1", "0|1", RESTART},
    {"MODS_REBUILD", "auto", "auto|off", RESTART},
    {"HIGHFPS_FIX", "1", "0|1", RESTART},
    {"LANG", "", "ja|en-US|fr|de|it|es|zh-CN|ko|nl|pt|ru|zh-TW|en-GB|fr-CA|es-419|zh-Hans|zh-Hant|pt-BR", RESTART},
    {"OPERATION_MODE", "docked", "docked|handheld", RESTART},
    {"ASSUME_FOCUS", "1", "0|1", LIVE},   // read on every focus change (ov_system.c)
    // Sound, Controls, Menu
    {"VOLUME", "100", "0..100", LIVE}, {"AUDIO_CHANNELS", "stereo", "auto|stereo|5.1", RESTART},
    // fastball: RUMBLE_STRENGTH 100 (TotK 130), the strength MLB had before the row was read (hid.c)
    {"MOTION", "1", "0|1", LIVE}, {"RUMBLE_STRENGTH", "100", "0..300", LIVE}, {"RUMBLE_CURVE", "0.6", "0.2..1.5", LIVE},
    {"RUMBLE_TRIGGERS", "0", "0|1", LIVE}, {"PAD_LAYOUT", "auto", "auto|nintendo|xbox", LIVE},
    {"PROMPTS", "nintendo", "nintendo|xbox|pc", RESTART},
    {"STICK_DEADZONE", "8", "0..50", LIVE}, {"STICK_SENS", "1", "0.1..3", RESTART},
    {"LSTICK_DEADZONE", "", "0..50", LIVE}, {"RSTICK_DEADZONE", "", "0..50", LIVE},
    {"MOUSE_MODE", "direct", "stick|direct", LIVE},
    {"MOUSE_SENS", "1", "0..10", LIVE}, {"MOUSE_AIM_SCALE", "1", "0.1..3", LIVE}, {"MENU_CURSOR", "1", "0|1", LIVE},
    {"WALK_KEY", "none", NULL, LIVE}, {"WALK_SCALE", "0.51", "0.2..0.9", LIVE},
    {"MENU_LANG", "", "en|ru|pt-BR|zh-Hans", LIVE},
    {"AMIIBO_DIR", "", NULL, LIVE},
    // Discord Rich Presence (discord_rpc.c): off by default, the Menu tab of the settings menu switches it.
    {"DISCORD_RPC", "0", "0|1", LIVE},
    {"DISCORD_APP_ID", "1567232599846293504", NULL, LIVE},
    {"DISCORD_IMAGE", "totk_512x512", NULL, LIVE},
    // Advanced: scheduling
    // fastball: REPLAY_ASYNC on (the batting frame rate): the replay runs on its own thread (vk_backend.c).
    // Synchronous, it ran on the game's main thread after the game's own work, and busy at-bat scenes took 6-9 ms of
    // replay on top of 10-12 ms of game work a frame (45-55 fps).
    {"ASYNC_PREEMPT", "1", "0|1|2", RESTART}, {"REPLAY_ASYNC", "1", "0|1", RESTART},
    {"FREE_CORES", "0", "0|1", RESTART}, {"TIMER_1MS", "1", "0|1", RESTART},
    // shaders and pipelines
    {"PIPE_WAIT", "default", "default|draw", RESTART}, {"PIPE_SKIP", "fullscreen", "strict|any|fullscreen", RESTART},
    {"LIB_PREBUILD", "1", "0|1", RESTART}, {"LIB_PREBUILD_THREADS", "", "1..64", RESTART},
    {"PIPE_PREBUILD", "1", "0|1", RESTART}, {"PREWARM_XLATE", "", "1..64", RESTART}, {"GPL", "1", "0|1", RESTART},
    // textures and memory
    {"ASTC", "auto", "auto|gpu|native|cpu", RESTART}, {"MEMORY", "auto", "auto|import|mirror|verify", RESTART},
    {"TEX_ALIAS", "1", "0|1", RESTART}, {"TEX_VRAM_PRESSURE", "90", "0..100", RESTART}, {"ASTC_BC", "", "0|1", RESTART}, {"ZSTD_NATIVE", "1", "0|1", RESTART}, {"ZSTD_CHECK", "0", "0|1", RESTART},
    // render fixes
    {"LOD_GUARD", "1", "0|1", RESTART}, {"FIX_C0", "1", "0|1", RESTART}, {"FIX_ZERO", "1", "0|1", RESTART},
    {"BIND_SKIP", "1", "0|1", RESTART}, {"SHADER_MEMO", "1", "0|1", RESTART}, {"PIPE_MEMO", "1", "0|1", RESTART}, {"TEX_MEMO", "1", "0|1", RESTART},
    {"VB_USED", "1", "0|1", RESTART},
    {"DESC_TEMPLATE", "1", "0|1", RESTART},
    {"PUSH_SKIP", "1", "0|1", RESTART}, {"SPLIT_DRAWS", "64", "0..1000", RESTART},
    {"SPLIT_US", "1000", "0..20000", RESTART},
    // Vulkan
    {"VK_VALIDATION", "0", "0|1", RESTART}, {"VK_DYN_VERTEX", "", "0|1", RESTART},
    {"VK_DYN_A2C", "", "0|1", RESTART}, {"VK_SUBGROUP_PIN", "1", "0|1", RESTART},
    // diagnostics
    {"TRACE", "", NULL, RESTART}, {"FNTRACE", "", NULL, RESTART}, {"FLIGHT", "0", "0|1", RESTART}, {"FLIGHT_SPIKE_MS", "100", "0..60000", RESTART},
    {"WB_FENCE_LIMIT", "65536", "0..1073741824", RESTART}, {"WB_MERGE", "1", "0|1", RESTART}, {"WB_EXACT", "1", "0|1", RESTART},
    {"PASS_CRUMBS", "", "0|1", RESTART},   // vk_crumbs.c pass marks; unset: VK_AMD_buffer_marker only
    {"STATEDUMP", "0", "0|1", RESTART},    // statedump.c: F8 writes main's data and bss to the profile folder
    {"CAPTURE_KEY", "0", "0|1", RESTART},  // gfx_stubs.c: F12 captures a frame (always F9 under RenderDoc)
    {"HEAP_CENSUS", "0", "0|1", RESTART},  // heap.c: with TRACE=perf, the malloc heap's live blocks by kind (#17)
    {"INPUT_RECORD", "", NULL, RESTART},
};

// Values set at run time (cfg_set): a small table under a lock; strings are never freed, as readers may hold them.
#define MAX_SET 256   // fastball: 64 in TotK; mlb.cfg and the menu's many rows set more
static struct { char key[64]; const char *value; } set_values[MAX_SET];
static int n_set;
static PlatMutex set_lock = PLAT_MUTEX_INIT;
static unsigned version = 1;

void cfg_init(void) {}

// "TOTK_X", "MLB_X" or "X" -> "X", upper case, into k (64 bytes).
static const char *norm_key(const char *name, char *k) {
    if (!strncmp(name, "TOTK_", 5)) name += 5;
    else if (!strncmp(name, "MLB_", 4)) name += 4;
    size_t i = 0;
    for (; name[i] && i < 63; i++) k[i] = (char)toupper((unsigned char)name[i]);
    k[i] = 0;
    return k;
}

const char *cfg_get(const char *name) {
    char k[64], var[80];
    norm_key(name, k);
    snprintf(var, sizeof var, "MLB_%s", k);
    const char *v = getenv(var);
    if (v) return v;
    snprintf(var, sizeof var, "TOTK_%s", k);
    if ((v = getenv(var))) return v;
    plat_mutex_lock_shared(&set_lock);
    for (int i = 0; i < n_set && !v; i++)
        if (!strcmp(set_values[i].key, k)) v = set_values[i].value;
    plat_mutex_unlock_shared(&set_lock);
    return v;
}

void cfg_set(const char *name, const char *value) {
    char k[64];
    norm_key(name, k);
#ifdef _WIN32
    const char *copy = value ? _strdup(value) : NULL;   // MSVC CRT name
#else
    const char *copy = value ? strdup(value) : NULL;
#endif
    plat_mutex_lock(&set_lock);
    int i = 0;
    while (i < n_set && strcmp(set_values[i].key, k)) i++;
    if (i == n_set) {
        if (n_set == MAX_SET) rt_fatal("cfg: more than %d settings set at run time", MAX_SET);
        snprintf(set_values[n_set++].key, sizeof set_values[0].key, "%s", k);
    }
    set_values[i].value = copy;
    version++;
    plat_mutex_unlock(&set_lock);
}

// mlb.cfg's path, beside the executable. Windows: UTF-16, so a folder name outside the ANSI code page still opens.
#ifdef _WIN32
typedef wchar_t PathCh;
#define PSTR(s) L##s
#define pfopen _wfopen
#define premove _wremove
#define pcat wcscat
#define pcpy wcscpy
#define plen wcslen
static int cfg_dir(PathCh *out, size_t cap) { return plat_exe_dir_wide(out, cap); }
static int preplace(const PathCh *from, const PathCh *to) { return MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING) != 0; }
#else
typedef char PathCh;
#define PSTR(s) s
#define pfopen fopen
#define premove remove
#define pcat strcat
#define pcpy strcpy
#define plen strlen
static int cfg_dir(PathCh *out, size_t cap) {
    char *cut = plat_exe_path(out, cap) ? strrchr(out, '/') : NULL;
    if (!cut) return 0;
    cut[1] = 0;
    return 1;
}
static int preplace(const PathCh *from, const PathCh *to) { return plat_file_replace(from, to); }
#endif

// fastball: settings from mlb.cfg beside the executable, one KEY=VALUE per line (MLB_X, TOTK_X or X; '#' or ';'
// starts a comment), so a player can switch an experiment without a new build or environment variables. Applied with
// cfg_set before anything reads a setting: the environment still wins, and the defaults main.c sets for unset keys
// leave a file's value alone. Returns how many settings it applied (0 without a file).
int cfg_load_file(void) {
    PathCh dir[1024];
    if (!cfg_dir(dir, 1000) || plen(dir) > 1000) return 0;
    pcat(dir, PSTR("mlb.cfg"));
    FILE *f = pfopen(dir, PSTR("r"));
    if (!f) return 0;
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        char *hash = strpbrk(p, "#;");
        if (hash) *hash = 0;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = p, *v = eq + 1, *e;
        for (e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); e--) *(e - 1) = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (e = v + strlen(v); e > v && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'); e--) *(e - 1) = 0;
        if (!*k) continue;
        cfg_set(k, v);
        rt_log("cfg: mlb.cfg: %s=%s%s", k, v, getenv(k) ? " (the environment overrides it)" : "");
        n++;
    }
    fclose(f);
    if (!n) rt_log("cfg: mlb.cfg read, no settings in it");
    return n;
}

unsigned cfg_version(void) { return version; }
int cfg_is_set(const char *name) { return cfg_get(name) != NULL; }

int cfg_from_env(const char *name) {
    char k[64], var[80];
    norm_key(name, k);
    snprintf(var, sizeof var, "MLB_%s", k);
    if (getenv(var)) return 1;
    snprintf(var, sizeof var, "TOTK_%s", k);
    return getenv(var) != NULL;
}

// fastball: the settings the menu changed (cfg_set_saved), which cfg_save writes into mlb.cfg.
#define MAX_SAVED 256
static char saved_keys[MAX_SAVED][64];
static int n_saved;

void cfg_set_saved(const char *name, const char *value) {
    char k[64];
    norm_key(name, k);
    cfg_set(k, value);
    plat_mutex_lock(&set_lock);
    int i = 0;
    while (i < n_saved && strcmp(saved_keys[i], k)) i++;
    if (i == n_saved && n_saved < MAX_SAVED) snprintf(saved_keys[n_saved++], sizeof saved_keys[0], "%s", k);
    plat_mutex_unlock(&set_lock);
}

// fastball: the menu's changes go into mlb.cfg beside the executable. A line that sets one of those keys (MLB_X,
// TOTK_X or X, not commented out) gets the new value in place; the others, comments included, stay as the player
// wrote them; keys without a line are added at the end under a heading. Written to mlb.cfg.tmp, then moved over
// mlb.cfg, so a crash mid-write leaves the old file. 1 when written (or nothing to write).
static int save_file(char (*keys)[64], char (*vals)[256], int n);
static int last_save_ok = 1;
int cfg_last_save_ok(void) { return __atomic_load_n(&last_save_ok, __ATOMIC_RELAXED); }

int cfg_save(void) {
    // one copy of the keys and values at a time: the menu (present path) and F11 (window thread) both save
    static PlatMutex save_lock = PLAT_MUTEX_INIT;
    static char keys[MAX_SAVED][64], vals[MAX_SAVED][256];
    plat_mutex_lock(&save_lock);
    plat_mutex_lock_shared(&set_lock);
    int n = n_saved;
    for (int i = 0; i < n; i++) {
        snprintf(keys[i], sizeof keys[i], "%s", saved_keys[i]);
        const char *v = NULL;
        for (int j = 0; j < n_set && !v; j++)
            if (!strcmp(set_values[j].key, keys[i])) v = set_values[j].value;
        snprintf(vals[i], sizeof vals[i], "%s", v ? v : "");
    }
    plat_mutex_unlock_shared(&set_lock);
    int ok = 1;
    if (n) ok = save_file(keys, vals, n);
    __atomic_store_n(&last_save_ok, ok, __ATOMIC_RELAXED);
    plat_mutex_unlock(&save_lock);
    return ok;
}

static int save_file(char (*keys)[64], char (*vals)[256], int n) {
    PathCh path[1024], tmp[1040];
    if (!cfg_dir(path, 1000) || plen(path) > 1000) return 0;
    pcat(path, PSTR("mlb.cfg"));
    pcpy(tmp, path);
    pcat(tmp, PSTR(".tmp"));
    FILE *in = pfopen(path, PSTR("r")), *out = pfopen(tmp, PSTR("w"));
    if (!out) {
        if (in) fclose(in);
        return 0;
    }
    static int done[MAX_SAVED];
    memset(done, 0, sizeof done);
    char line[512];
    while (in && fgets(line, sizeof line, in)) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        const char *eq = strchr(p, '=');
        int hit = -1;
        if (*p != '#' && *p != ';' && eq && eq - p < 63) {
            char name[64], k[64];
            size_t len = (size_t)(eq - p);
            memcpy(name, p, len);
            while (len && (name[len - 1] == ' ' || name[len - 1] == '\t')) len--;
            name[len] = 0;
            norm_key(name, k);
            for (int i = 0; i < n && hit < 0; i++)
                if (!strcmp(keys[i], k)) hit = i;
        }
        if (hit >= 0) {
            fprintf(out, "MLB_%s=%s\n", keys[hit], vals[hit]);
            done[hit] = 1;
        } else {
            fputs(line, out);
            size_t l = strlen(line);
            if (l && line[l - 1] != '\n') fputc('\n', out);
        }
    }
    if (in) fclose(in);
    int head = 0;
    for (int i = 0; i < n; i++) {
        if (done[i]) continue;
        if (!head++) fputs("\n# --- Set in the in-game menu (F1) ----------------------------------------------\n", out);
        fprintf(out, "MLB_%s=%s\n", keys[i], vals[i]);
    }
    int ok = !ferror(out);
    ok &= fclose(out) == 0;
    if (ok) ok = preplace(tmp, path);
    if (!ok) premove(tmp);
    rt_log("cfg: %s mlb.cfg (%d setting%s from the menu)", ok ? "wrote" : "could not write", n, n == 1 ? "" : "s");
    return ok;
}

int cfg_reset(const char *const *keys, int n) {
    int done = 0;
    for (int i = 0; i < n; i++) {
        const CfgMeta *m = cfg_meta(keys[i]);
        if (!m || cfg_from_env(m->key)) continue;
        const char *now = cfg_get(m->key);
        if (now && strcmp(now, m->def)) {
            cfg_set_saved(m->key, m->def);
            done++;
        }
    }
    return done;
}

const CfgMeta *cfg_meta_at(int i) { return i >= 0 && i < (int)(sizeof metas / sizeof *metas) ? &metas[i] : NULL; }

void cfg_each_active(void (*fn)(const char *key, const char *value, int from_env, void *ctx), void *ctx) {
    for (size_t i = 0; i < sizeof metas / sizeof *metas; i++) {
        const char *v = cfg_get(metas[i].key);
        if (v && *v) fn(metas[i].key, v, cfg_from_env(metas[i].key), ctx);
    }
}

const CfgMeta *cfg_meta(const char *key) {
    char k[64];
    norm_key(key, k);
    for (size_t i = 0; i < sizeof metas / sizeof *metas; i++)
        if (!strcmp(metas[i].key, k)) return &metas[i];
    return NULL;
}

const char *cfg_value(const char *name) {
    const char *e = cfg_get(name);
    if (e && *e) return e;
    const CfgMeta *m = cfg_meta(name);
    if (!m) rt_fatal("cfg: %s has no row in the settings table", name);
    return m->def;
}

static double default_number(const char *name) { return strtod(cfg_meta(name)->def, NULL); }

double cfg_double(const char *name) {
    const char *v = cfg_value(name);
    char *end;
    double d = strtod(v, &end);
    return end == v || *end || d != d ? default_number(name) : d;
}

int cfg_int(const char *name) {
    const char *v = cfg_value(name);
    char *end;
    long l = strtol(v, &end, 10);
    return end == v || *end ? (int)default_number(name) : (int)l;
}

int cfg_bool(const char *name) { return strcmp(cfg_value(name), "0") != 0; }
