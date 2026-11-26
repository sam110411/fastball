// Adapted from TotK Recomp runtime/main.c @ ae30a61 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), which adapts LAS
// Recomp runtime/main.c @ da35b83 (MIT, Copyright (c) 2026 Peiikai). Kept: logging, the runtime services the
// generated code calls (cpu.h), the crash reporter. Dropped: TotK's settings file, probes, Discord, play time,
// resolution hooks and the graphics modes.
//
// mlb.exe [--game DIR | --exefs DIR --romfs DIR] [--profile DIR] [--fullscreen] [--<setting> VALUE ...]: --help lists
// them (USAGE below).
// Without arguments the paths come from MLB_EXEFS / MLB_ROMFS / MLB_PROFILE, else the defaults tools/build.py bakes in:
// the repository's work/update/exefs, work/romfs/romfs and work/profile for a dev build, game/exefs, game/romfs and
// profile beside mlb.exe for an installed one (--portable, docs/release.md).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "platform/platform.h"
#include "sha256.h"

// ---- log: stderr, plus logs/mlb.log next to mlb.exe once log_open ran. Lines before log_open (mem_init runs first,
// so the CRT heap does not land in the guest address space) wait in a static buffer.
#define LOG_CAP (64ull << 20)
enum { LOG_EARLY, LOG_FILE, LOG_OFF };
static int g_log_state;
static PlatAppend *g_log_file;
static char g_log_path[PLAT_PATH_MAX * 3];
static char g_log_early[32768];
static size_t g_log_early_n;
static uint64_t g_log_bytes;
unsigned rt_trace;

void rt_log_raw(const char *p, size_t n) {
    fwrite(p, 1, n, stderr);
    int state = __atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE);
    if (state == LOG_EARLY) {
        size_t at = __atomic_fetch_add(&g_log_early_n, n, __ATOMIC_RELAXED);
        if (at + n <= sizeof g_log_early) memcpy(g_log_early + at, p, n);
    } else if (state == LOG_FILE) {
        uint64_t at = __atomic_fetch_add(&g_log_bytes, n, __ATOMIC_RELAXED);
        if (at + n <= LOG_CAP) plat_append_write(g_log_file, p, n);
    }
}

void rt_vlogf(const char *fmt, va_list ap) {
    char buf[2048];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0) rt_log_raw(buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
}

void rt_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlogf(fmt, ap);
    va_end(ap);
}

void rt_log(const char *fmt, ...) {
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "[host] ");
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    n = k < 0 ? n : n + k < (int)sizeof buf - 1 ? n + k : (int)sizeof buf - 2;
    buf[n++] = '\n';
    rt_log_raw(buf, n);
}

const char *rt_log_path(void) {
    return __atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE) == LOG_FILE ? g_log_path : NULL;
}

static void log_open(void) {
    char dir[PLAT_PATH_MAX * 3];
    char *cut = NULL;
    if (plat_exe_path(dir, sizeof dir)) {
        cut = plat_last_sep(dir);
        char *f = strrchr(dir, '/');
        if (f > cut) cut = f;
    }
    if (cut && (size_t)(cut - dir) + sizeof "/logs" <= sizeof dir) {
        strcpy(cut, "/logs");
        plat_file_mkdir_p(dir);
        snprintf(g_log_path, sizeof g_log_path, "%s/mlb.log", dir);
        g_log_file = plat_append_open(g_log_path);
    }
    if (!g_log_file) {
        __atomic_store_n(&g_log_state, LOG_OFF, __ATOMIC_RELEASE);
        return;
    }
    size_t n = __atomic_load_n(&g_log_early_n, __ATOMIC_RELAXED);
    if (n > sizeof g_log_early) n = sizeof g_log_early;
    plat_append_write(g_log_file, g_log_early, n);
    g_log_bytes = n;
    __atomic_store_n(&g_log_state, LOG_FILE, __ATOMIC_RELEASE);
    rt_log("log: %s", g_log_path);
}

// MLB_TRACE (or TOTK_TRACE) = svc,host,ipc,fs,gfx,perf,sched,audio,present,all
static void parse_trace(void) {
    const char *e = cfg_get("TOTK_TRACE");
    if (!e) return;
    static const struct { const char *name; unsigned bit; } names[] = {
        {"svc", RT_TRACE_SVC}, {"host", RT_TRACE_HOSTCALL}, {"ipc", RT_TRACE_IPC}, {"fs", RT_TRACE_FS},
        {"gfx", RT_TRACE_GFX}, {"perf", RT_TRACE_PERF}, {"sched", RT_TRACE_SCHED}, {"audio", RT_TRACE_AUDIO},
        {"present", RT_TRACE_PRESENT}, {"all", ~0u}};
    for (const char *p = e; *p;) {
        size_t n = strcspn(p, ",");
        int known = 0;
        for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
            if (strlen(names[i].name) == n && !strncmp(p, names[i].name, n)) rt_trace |= names[i].bit, known = 1;
        if (!known && n) rt_log("MLB_TRACE: unknown item '%.*s'", (int)n, p);
        p += n + (p[n] == ',');
    }
}

void rt_sha256(const void *p, size_t n, uint8_t out[32]) { sha256(p, n, out); }

static RtConfig cfg;
const RtConfig *rt_config(void) { return &cfg; }

// ---- runtime services the generated code calls (cpu.h)
// MLB_FNTRACE=<guest addr>,... (diagnostic): logs the first calls of these game functions, then every 1000th, with the
// caller and x0. Every generated function checks g_fnprof on entry (FN_ENTER), so it stays 0 unless this is set: the
// game runs measurably slower while it is on. Used to follow the audio engine's stream threads (Scream snd_stream).
volatile int g_fnprof = 0;
static uint64_t fn_addr[16];
static uint64_t fn_hits[16];
static int fn_n;
static uint64_t fn_lo, fn_hi;
// MLB_FNTRACE_PEEK=<off>,...: also a 32-bit value behind x0 (x0 + off, read as a pointer for every offset but the last)
static uint64_t fn_peek[4];
static int fn_npeek;
static int fn_peek_value(uint64_t x0, uint32_t *out) {
    uint64_t p = x0;
    for (int i = 0; i < fn_npeek; i++) {
        uint64_t a = p + fn_peek[i];
        if (!plat_mem_readable(a, 8)) return 0;
        if (i == fn_npeek - 1) {
            *out = *(const uint32_t *)(uintptr_t)a;
            return 1;
        }
        p = *(const uint64_t *)(uintptr_t)a;
    }
    return 0;
}   // MLB_FNTRACE=<lo>-<hi>: the first call of every function in [lo, hi)
void fnprof_hit(uint64_t addr, uint64_t lr, uint64_t x0) {
    if (addr >= fn_lo && addr < fn_hi) {
        static uint64_t first[65536];
        for (unsigned j = 0, h = (unsigned)((addr * 0x9e3779b97f4a7c15ull) >> 48); j < 65536; j++, h = (h + 1) & 65535) {
            uint64_t v = __atomic_load_n(&first[h], __ATOMIC_RELAXED);
            if (v == addr) return;
            if (!v) {
                if (!__atomic_compare_exchange_n(&first[h], &v, addr, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
                char from[128];
                rt_log("fntrace: first call of %#llx from %s, x0 %#llx", (unsigned long long)addr,
                       ldr_describe(lr, from, sizeof from), (unsigned long long)x0);
                return;
            }
        }
        return;
    }
    for (int i = 0; i < fn_n; i++) {
        if (fn_addr[i] != addr) continue;
        uint64_t k = __atomic_add_fetch(&fn_hits[i], 1, __ATOMIC_RELAXED);
        uint32_t peek = 0;
        int peeked = fn_npeek && fn_peek_value(x0, &peek);
        if (peeked) {   // the value behind x0 changed since this function's last call with this x0
            static uint64_t last_x0[64];
            static uint32_t last_v[64];
            unsigned h = (unsigned)((x0 * 0x9e3779b97f4a7c15ull) >> 58);
            if (last_x0[h] != x0 || last_v[h] != peek) {
                last_x0[h] = x0, last_v[h] = peek;
                rt_log("fntrace: %#llx call %llu, x0 %#llx: peek %u (%#x)", (unsigned long long)addr,
                       (unsigned long long)k, (unsigned long long)x0, peek, peek);
            }
            return;
        }
        // a call with an x0 not seen before is logged too (each distinct message of a print function, once)
        static uint64_t seen[4096];
        uint64_t key = (addr * 0x9e3779b97f4a7c15ull) ^ x0 ^ (x0 >> 29), fresh = 0;
        for (unsigned j = 0, h = (unsigned)(key >> 52); j < 4096; j++, h = (h + 1) & 4095) {
            uint64_t v = __atomic_load_n(&seen[h], __ATOMIC_RELAXED);
            if (v == key) break;
            if (!v) {
                fresh = __atomic_compare_exchange_n(&seen[h], &v, key, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
                break;
            }
        }
        if (k <= 8 || k % 1000 == 0 || fresh) {
            char from[128];
            // x0 into main's read-only data (1.0.15: 0x7001c22000 + 0x539000, docs/recon-report.md; 1.0.20 starts
            // 0x1000 later): a message, shown as text
            const RtLoaded *m = ldr_module(0);
            int text = m && x0 >= m->base + ((m->text_end + 0xfff) & ~0xfffull) && x0 < m->base + m->data_off;
            rt_log("fntrace: %#llx call %llu from %s, x0 %#llx%s%.160s%s", (unsigned long long)addr, (unsigned long long)k,
                   ldr_describe(lr, from, sizeof from), (unsigned long long)x0, text ? " \"" : "",
                   text ? (const char *)(uintptr_t)x0 : "", text ? "\"" : "");
        }
        return;
    }
}

static void parse_fntrace(void) {
    const char *pk = getenv("MLB_FNTRACE_PEEK");
    for (const char *p = pk; p && *p && fn_npeek < 4;) {
        char *end;
        fn_peek[fn_npeek] = strtoull(p, &end, 16);
        if (end == p) break;
        fn_npeek++;
        p = end + (*end == ',');
    }
    const char *e = cfg_get("TOTK_FNTRACE");
    if (!e || !*e) return;
    for (const char *p = e; *p && fn_n < 16;) {
        char *end;
        uint64_t a = strtoull(p, &end, 16);
        if (end == p) break;
        if (*end == '-') {
            const char *q = end + 1;
            fn_lo = a, fn_hi = strtoull(q, &end, 16);
            if (end == q) break;
            p = end + (*end == ',');
            continue;
        }
        fn_addr[fn_n++] = a;
        p = end + (*end == ',');
    }
    if (fn_n || fn_hi > fn_lo) {
        rt_log("MLB_FNTRACE: following %d game functions and the range %#llx-%#llx (the game runs slower)", fn_n,
               (unsigned long long)fn_lo, (unsigned long long)fn_hi);
        g_fnprof = 1;
    }
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    char at[128], lr[128];
    rt_fatal("brk #%#x at %s (lr %s)", imm, ldr_describe(pc, at, sizeof at), ldr_describe(c->x[30], lr, sizeof lr));
}

void rt_udf(Cpu *c, uint64_t pc) {
    char at[128], lr[128];
    rt_fatal("undefined instruction at %s (lr %s)", ldr_describe(pc, at, sizeof at),
             ldr_describe(c->x[30], lr, sizeof lr));
}

// CNTPCT_EL0: the Switch counter runs at 19.2 MHz.
static PlatScale tick_scale;
__attribute__((constructor)) static void tick_scale_init(void) { tick_scale = plat_scale(19200000u, plat_counter_hz()); }
uint64_t rt_ticks(void) { return plat_scaled(tick_scale, plat_counter()); }

// ---- crash reporter
typedef struct { uintptr_t host; uint64_t guest; } FnRev;
static FnRev *fn_rev;
static uint64_t fn_rev_n;

static int by_host(const void *a, const void *b) {
    uintptr_t x = ((const FnRev *)a)->host, y = ((const FnRev *)b)->host;
    return x < y ? -1 : x > y;
}

#define MAX_MODULES 16
static uint64_t lifted_function(uintptr_t rip) {
    if (!fn_rev) {
        static RtModule mods[MAX_MODULES];
        int n = rt_linked_modules(mods, MAX_MODULES);
        uint64_t total = 0;
        for (int i = 0; i < n; i++) total += mods[i].count;
        fn_rev = malloc((total ? total : 1) * sizeof *fn_rev);
        if (!fn_rev) return 0;
        for (int i = 0; i < n; i++)
            for (uint64_t k = 0; k < mods[i].count; k++)
                fn_rev[fn_rev_n++] = (FnRev){(uintptr_t)mods[i].fns[k], mods[i].addrs[k]};
        qsort(fn_rev, fn_rev_n, sizeof *fn_rev, by_host);
    }
    if (!fn_rev_n || rip > fn_rev[fn_rev_n - 1].host) return 0;
    uint64_t lo = 0, hi = fn_rev_n;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (fn_rev[mid].host <= rip) lo = mid + 1;
        else hi = mid;
    }
    return lo ? fn_rev[lo - 1].guest : 0;
}

static void crash_handler(const PlatCrash *x) {
    static volatile int32_t busy;
    if (plat_xchg32(&busy, 1)) plat_exit(71);
    char d[160];
    uintptr_t rip = x->rip;
    rt_logf("FATAL: %s (%#x) at host %p (", x->what, x->code, (void *)rip);
    if (x->module && !*x->module) rt_logf("outside every module)");
    else rt_logf("%s+%#llx)", x->module ? x->module : "mlb", (unsigned long long)x->offset);
    if (x->access >= 0)
        rt_logf(", %s address %#llx", x->access == 1 ? "writing" : x->access == 8 ? "executing" : "reading",
                (unsigned long long)x->addr);
    rt_log_raw("\n", 1);
    uint64_t f = x->module ? 0 : lifted_function(rip);
    if (f) rt_logf("  in lifted function f_%llx = %s\n", (unsigned long long)f, ldr_describe(f, d, sizeof d));
    uintptr_t exe = plat_exe_base();
    for (int i = 0; i < x->nframes; i++) {
        const char *m = x->frame_module[i];
        if (m) {
            rt_logf("  host frame %2d: %s+%#llx\n", i, *m ? m : "outside every module",
                    (unsigned long long)x->frame_offset[i]);
            continue;
        }
        uint64_t g = lifted_function(x->frames[i]);
        rt_logf("  host frame %2d: mlb+%#llx", i, (unsigned long long)(x->frames[i] - exe));
        if (g) rt_logf(" in lifted function f_%llx", (unsigned long long)g);
        rt_log_raw("\n", 1);
    }
    Cpu *c = kern_cpu();
    if (c) {
        for (int i = 0; i < 31; i++)
            rt_logf("  x%-2d %016llx%s", i, (unsigned long long)c->x[i], i % 4 == 3 ? "\n" : "");
        rt_logf("  sp  %016llx\n  pc  %016llx", (unsigned long long)c->sp, (unsigned long long)c->pc);
        if (hostfn_name(c->pc)) rt_logf(" (%s)", ldr_describe(c->pc, d, sizeof d));
        rt_logf("  lr %s\n", ldr_describe(c->x[30], d, sizeof d));
        uint64_t fp = c->x[29];
        for (int n = 0; n < 32 && fp && !(fp & 7) && plat_mem_readable(fp, 16); n++) {
            uint64_t next = LD64(fp), lr = LD64(fp + 8);
            rt_logf("  guest frame %2d: %s\n", n, ldr_describe(lr, d, sizeof d));
            if (next <= fp || next - fp > (64u << 20)) break;
            fp = next;
        }
    } else {
        rt_logf("  not on a guest thread\n");
    }
    fflush(stderr);
    char what[200];   // an installed (windowed) game tells the player; the log has the frames
    snprintf(what, sizeof what, "%s (%#x) at %s+%#llx", x->what, x->code, x->module && *x->module ? x->module : "mlb",
             (unsigned long long)x->offset);
    plat_fatal_notice(what);
    plat_exit(71);
}

// ---- command line. fastball: every setting is also a flag, so a launcher (RecompLauncher, a shortcut, a script)
// can start the game the way it wants without writing mlb.cfg: --fullscreen, --fps-limit 120, --set KEY=VALUE. A flag
// is for this run: it beats mlb.cfg and the environment, and is not written into mlb.cfg (the in-game menu still saves
// what the player changes there). A self-restart (plat_restart) starts with the same command line.
static const char USAGE[] =
    "usage: mlb.exe [options]\n"
    "\n"
    "Game files (default: game/exefs, game/romfs and profile beside mlb.exe):\n"
    "  --game DIR             the extracted game: DIR/exefs and DIR/romfs\n"
    "  --exefs DIR            the game's code (main, sdk, rtld, main.npdm)\n"
    "  --romfs DIR            the game's data\n"
    "  --profile DIR          saves, account and play time\n"
    "\n"
    "Settings, for this run (the same keys as mlb.cfg; mlb.cfg and the in-game menu keep theirs):\n"
    "  --fullscreen, --windowed\n"
    "  --<setting> VALUE      any setting below: --fps-limit 120, --output-res 2560x1440, --dlss quality\n"
    "  --<setting>=VALUE      the same\n"
    "  --<setting>, --no-<setting>   a 0|1 setting on or off: --hdr, --no-rumble\n"
    "  --set KEY=VALUE        any key, as mlb.cfg writes it (MLB_FPS_LIMIT=120 or FPS_LIMIT=120)\n"
    "\n"
    "Other:\n"
    "  --import-save [ryujinx | FOLDER]   copy another host's save of the game into the profile, then exit\n"
    "  --probe-fs             test the file system overrides, then exit\n"
    "  --boot                 load the game and stop before it runs\n"
    "  --version              the port's and the game's version\n"
    "  --help                 this text and every setting\n";

static void print_settings(FILE *f) {
    fputs("\nSettings (flag, values, default):\n", f);
    for (int i = 0; cfg_meta_at(i); i++) {
        const CfgMeta *m = cfg_meta_at(i);
        if (cfg_meta(m->key) != m) continue;   // a key's second row (PROMPTS): the first one is the one read
        char flag[64];
        size_t n = 0;
        for (; m->key[n] && n < sizeof flag - 1; n++) flag[n] = m->key[n] == '_' ? '-' : (char)tolower((unsigned char)m->key[n]);
        flag[n] = 0;
        fprintf(f, "  --%-22s %-34s %s\n", flag, m->values ? m->values : "(text)", *m->def ? m->def : "(auto)");
    }
}

// Shows text a command-line answer gives: on the console the game was started from, else in a message box (a shortcut
// with a wrong flag must not end the game without a word).
static void cli_say(int error, const char *text, int with_settings) {
    if (plat_console_attach()) {
        fputs(text, error ? stderr : stdout);
        if (with_settings) print_settings(error ? stderr : stdout);
        return;
    }
    char box[2400];
    snprintf(box, sizeof box, "%s%s", text, with_settings ? "\nmlb.exe --help in a console window lists every "
             "setting." : "");
    plat_message(error ? "Fastball: wrong command line" : "Fastball", box);
}

static int usage_error(const char *fmt, const char *arg) {
    char text[sizeof USAGE + 300];
    int n = snprintf(text, 300, fmt, arg);
    if (n < 0 || n >= 300) n = 299;
    snprintf(text + n, sizeof text - n, "\n\n%s", USAGE);
    cli_say(1, text, 0);
    return 2;
}

// "--fps-limit" -> "FPS_LIMIT" (k, 64 bytes); the setting's row, or NULL
static const CfgMeta *flag_setting(const char *flag, size_t len, char *k) {
    size_t n = 0;
    for (; n < len && n < 63; n++) k[n] = flag[n] == '-' ? '_' : (char)toupper((unsigned char)flag[n]);
    k[n] = 0;
    return n ? cfg_meta(k) : NULL;
}

// A setting from the command line: cfg_set (this run, not saved), and the environment's MLB_X / TOTK_X removed, which
// would otherwise win over it.
static void cli_set(const char *key, const char *value) {
    char k[64], var[80];
    snprintf(k, sizeof k, "%s", !strncmp(key, "MLB_", 4) ? key + 4 : !strncmp(key, "TOTK_", 5) ? key + 5 : key);
    for (char *p = k; *p; p++) *p = (char)toupper((unsigned char)*p);
    snprintf(var, sizeof var, "MLB_%s", k);
    plat_setenv(var, NULL);
    snprintf(var, sizeof var, "TOTK_%s", k);
    plat_setenv(var, NULL);
    cfg_set(k, value);
    rt_log("cfg: command line: %s=%s%s", k, value, cfg_meta(k) ? "" : " (no setting of that name in the table)");
}

// A relative default path (an installed folder: tools/build.py --portable bakes game/exefs, game/romfs and profile) is
// taken beside mlb.exe, so the folder works wherever the player put it and whatever the working directory.
static const char *beside_exe(const char *p) {
    static char bufs[3][PLAT_PATH_MAX];
    static int used;
    char exe[PLAT_PATH_MAX];
    if (!p[0] || p[0] == '/' || p[0] == '\\' || p[1] == ':' || used == 3 || !plat_exe_path(exe, sizeof exe)) return p;
    char *cut = plat_last_sep(exe);
    if (!cut) return p;
    *cut = 0;
    char *b = bufs[used++];
    snprintf(b, sizeof bufs[0], "%s/%s", exe, p);
    return b;
}

int main(int argc, char **argv) {
    mem_init();   // first: claim the guest address space before anything else can land there
    rt_log("version: Fastball %s, game MLB The Show 26 %s", rt_port_version, rt_game_build);
    cpucheck_log();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    plat_crash_hook(crash_handler);
    cfg_load_file();   // mlb.cfg beside the exe: before any setting is read (the defaults below leave its values alone)
    const char *exefs = NULL, *romfs = NULL, *profile = NULL, *game = NULL;
    int probe = 0, import = 0, boot = 0;
    const char *import_from = NULL;
    for (int i = 1; i < argc; i++) {   // before anything reads a setting: the flags below are settings too
        const char *a = argv[i], **path = !strcmp(a, "--exefs") ? &exefs : !strcmp(a, "--romfs") ? &romfs
                                        : !strcmp(a, "--profile") ? &profile : !strcmp(a, "--game") ? &game : NULL;
        if (path) {
            if (++i == argc) return usage_error("%s needs a folder.", a);
            *path = argv[i];
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h") || !strcmp(a, "/?")) {
            char text[sizeof USAGE + 200];
            snprintf(text, sizeof text, "Fastball %s: MLB The Show 26 for PC, game version %s\n\n%s", rt_port_version,
                     rt_game_build, USAGE);
            cli_say(0, text, 1);
            return 0;
        } else if (!strcmp(a, "--version")) {
            char text[200];
            snprintf(text, sizeof text, "Fastball %s, MLB The Show 26 %s\n", rt_port_version, rt_game_build);
            cli_say(0, text, 0);
            return 0;
        } else if (!strcmp(a, "--boot")) {
            boot = 1;
        } else if (!strcmp(a, "--probe-fs")) {   // fs_host.c: the nn::fs overrides on the RomFS and in a temporary
            probe = 1;                           // profile; no guest code runs
        } else if (!strcmp(a, "--import-save")) {   // save_import.c: another host's save of the game (Ryujinx's by
            import = 1;                              // default) becomes the profile's save; no guest code runs
            if (i + 1 < argc && strncmp(argv[i + 1], "--", 2)) import_from = argv[++i];
        } else if (!strcmp(a, "--fullscreen") || !strcmp(a, "--windowed")) {
            cli_set("FULLSCREEN", a[2] == 'f' ? "1" : "0");
        } else if (!strcmp(a, "--set")) {
            const char *eq = i + 1 < argc ? strchr(argv[i + 1], '=') : NULL;
            if (!eq || eq == argv[i + 1]) return usage_error("--set needs KEY=VALUE%s.", "");
            char k[64];
            snprintf(k, sizeof k, "%.*s", (int)(eq - argv[i + 1]), argv[i + 1]);
            cli_set(k, eq + 1);
            i++;
        } else if (!strncmp(a, "--", 2) && a[2]) {   // --<setting> VALUE, --<setting>=VALUE, --<setting>, --no-<setting>
            const char *name = a + 2, *eq = strchr(name, '=');
            size_t len = eq ? (size_t)(eq - name) : strlen(name);
            char k[64];
            const CfgMeta *m = flag_setting(name, len, k);
            int off = 0;
            if (!m && !eq && !strncmp(name, "no-", 3) && (m = flag_setting(name + 3, len - 3, k)))
                off = 1;
            if (!m) return usage_error("Unknown option %s (mlb.exe --help lists every setting).", a);
            int onoff = m->values && !strcmp(m->values, "0|1");
            if (off && !onoff) return usage_error("%s: only a 0|1 setting has a --no- form.", a);
            if (eq) cli_set(k, eq + 1);
            else if (off) cli_set(k, "0");
            else if (i + 1 < argc && strncmp(argv[i + 1], "--", 2)) cli_set(k, argv[++i]);
            else if (onoff) cli_set(k, "1");
            else return usage_error("%s needs a value (mlb.exe --help lists them).", a);
        } else {
            return usage_error("Unknown argument %s.", a);
        }
    }
    // fastball: no tracing unless MLB_TRACE asks for it. Releases had perf,sched on for every player (from the
    // batting frame-rate work): counting and timing in the replay and the scheduler, and lines every second in mlb.log.
    // The developer's own mlb.cfg sets MLB_TRACE.
    // REPLAY_ASYNC and FRAME_QUEUE are on by default: their rows in cfg.c say why (the settings menu's reset needs the
    // defaults in the table)
    parse_trace();
    parse_fntrace();
    static char game_exefs[PLAT_PATH_MAX], game_romfs[PLAT_PATH_MAX];
    if (game) {
        snprintf(game_exefs, sizeof game_exefs, "%s/exefs", game);
        snprintf(game_romfs, sizeof game_romfs, "%s/romfs", game);
    }
    cfg = (RtConfig){.exefs = exefs ? exefs : game ? game_exefs : cfg_get("MLB_EXEFS") ? cfg_get("MLB_EXEFS")
                            : beside_exe(rt_default_exefs),
                     .romfs = romfs ? romfs : game ? game_romfs : cfg_get("MLB_ROMFS") ? cfg_get("MLB_ROMFS")
                            : beside_exe(rt_default_romfs),
                     .profile = profile ? profile : cfg_get("MLB_PROFILE") ? cfg_get("MLB_PROFILE")
                              : beside_exe(rt_default_profile),
                     .gpu = cfg_value("MLB_GPU"), .boot = boot};
    // --import-save reports on the console only: opening the log would replace mlb.log, a running game's too
    if (import) {
        plat_console_attach();   // started from a console window: its report shows there
        return save_import_main(import_from);
    }
    log_open();
    if (probe) return probe_fs();
    // fastball: the sampling profiler only when asked (it suspends every thread to sample it). MLB_SAMPLER=on: every
    // thread's call chains for each frame of 19 ms or more (one every 3 s at most) in <profile>/prof.txt.f<frame>;
    // MLB_SAMPLER=<path>: a profile there; off or unset: none.
    // The path goes to SAMPLER_OUT (prof_win.c), which an MLB_SAMPLER in the environment cannot shadow.
    const char *smp = cfg_get("MLB_SAMPLER");
    if (smp && (!strcmp(smp, "on") || !strcmp(smp, "1"))) {
        static char sp[1100];
        snprintf(sp, sizeof sp, "%s/prof.txt", cfg.profile);
        cfg_set("SAMPLER_OUT", sp);
        if (!cfg_get("MLB_SAMPLER_SLOW")) cfg_set("SAMPLER_SLOW", "19");
    } else if (smp && *smp && strcmp(smp, "off") && strcmp(smp, "0")) cfg_set("SAMPLER_OUT", smp);
    prof_start();   // perf.c: the frame-time summary, MLB_TRACE=perf, MLB_SAMPLER
    playtime_init();   // playtime.c: the settings menu's play time, in <profile>/playtime.txt
    rt_boot();
    return 0;
}
