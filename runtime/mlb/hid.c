// nn::hid by symbol override: the functions main imports (docs/runtime-port.md), fed by one host controller. Our own
// implementation; state layouts and bit numbers per libnx hid.h (ISC), read for the layout only; the override approach
// and the controller support result after TotK Recomp runtime/ov_input.c @ ae30a61 (MIT).
//
// The console is docked (am.c), so the controller is a full-key controller as player 1 (NpadId No1, style bit 0); the
// handheld id and the Joy-Con styles read as disconnected. Its state is merged from:
//   - every connected gamepad, through SDL3 (SDL3.dll next to mlb.exe, loaded at run time; tools/fetch_deps.py):
//     Xbox pads (SDL's XInput driver), PlayStation pads (DualSense, DualSense Edge, DualShock 4, DualShock 3; SDL's
//     HID drivers, USB or Bluetooth), Switch Pro and other pads SDL knows. Their buttons are ORed and each stick takes
//     the pad pushed furthest, so an Xbox and a PlayStation pad can be connected at once. As in TotK, Astral Chain and
//     Power Moon Recomp, which read their controllers through SDL3 too. MLB_PAD_DRIVER=xinput, or no SDL3.dll, reads
//     the first XInput pad instead (xinput1_4.dll; Xbox and compatible pads only).
//     MLB_PAD_MAP=auto (default) maps the face buttons by label in menus and by place in play, switching on the
//     screens the game enters (hid_game_log), so menus confirm with Xbox A / cross and gameplay matches PlayStation
//     and Xbox; the button icons follow (prompts.c). =position maps by place, as on the console: the east button (Xbox B,
//     circle) is Switch A, south (Xbox A, cross) B, west (Xbox X, square) Y, north (Xbox Y, triangle) X. The game
//     works in PlayStation buttons and its Switch build maps them by place (main+0x1343380, after GetNpadState:
//     Switch A to circle, B to cross, X to triangle, Y to square), so by place every gameplay control sits where it
//     does on PlayStation and Xbox: throw to first with the east button, home with the south one, power swing with
//     the west one. Its menus confirm with circle on Switch (BUTTON_MASK_UI_ACCEPT is 0x2000, main+0x4a36f4), so the
//     east button confirms and the south one goes back. =label maps by the letter or symbol: Xbox A / PlayStation
//     cross is Switch A (confirm with the south button), B / circle B, X / square X, Y / triangle Y (a
//     Nintendo-labelled pad's A is A); gameplay controls then move: east and south trade places, and so do west and
//     north (a throw to first on Xbox A, power swing on Xbox Y). Triggers past half way are ZL/ZR; View / Share /
//     Create and the PlayStation touchpad click are -, Menu / Options are +. The guide button (Xbox, PS) opens the
//     settings menu.
//   - the keyboard, while a window of this process is in the foreground (keys.h: GetAsyncKeyState on Windows, SDL's
//     keyboard state with the SDL window):
//       left stick W A S D, right stick I J K L, d-pad arrow keys, A Enter or Space, B Escape or Backspace, X R, Y F,
//       L Q, R E, ZL 1, ZR 3, + Tab, - M, left stick click C, right stick click V.
// Vibration plays on every pad's motors (MLB_RUMBLE=0: off): the low band's amplitude on the left (heavy) motor, the
// high band's on the right, times MLB_RUMBLE_STRENGTH percent. The sticks get a radial dead zone
// (MLB_LSTICK_DEADZONE / MLB_RSTICK_DEADZONE, else MLB_STICK_DEADZONE, percent; the settings menu's Controls tab).
// Nothing here allocates guest memory or calls back into the guest.
// On Linux SDL3 is linked (libSDL3.so.0, which the window uses too) and there is no XInput.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>   // types and constants only: the functions come from SDL3.dll (sdl_load)

#include "cfg.h"
#include "host.h"
#include "keys.h"
#include "platform/platform.h"
#include "prompts.h"
#include "swkbd.h"
int menu_is_open(void);   // ui/menu.cpp (ui/menu.h pulls in Vulkan)
void menu_toggle(void);

// ---- libnx layouts
typedef struct { uint64_t sampling, buttons; int32_t lx, ly, rx, ry; uint32_t attributes, reserved; } NpadState;
typedef struct { int8_t players; uint8_t pad[3]; uint32_t selected; } ControllerSupportResult;
typedef struct { float amp_low, freq_low, amp_high, freq_high; } VibrationValue;
typedef struct { uint32_t type, position; } VibrationDeviceInfo;
_Static_assert(sizeof(NpadState) == 40, "nn::hid::NpadFullKeyState");
_Static_assert(sizeof(ControllerSupportResult) == 8 && sizeof(VibrationValue) == 16, "nn::hid layouts");

enum { STYLE_FULL_KEY = 1u << 0 };
enum { ATTR_CONNECTED = 1u << 0, ATTR_WIRED = 1u << 1 };
enum { ID_NO1 = 0, ID_HANDHELD = 0x20 };
enum {
    B_A, B_B, B_X, B_Y, B_LSTICK, B_RSTICK, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS, B_LEFT, B_UP, B_RIGHT, B_DOWN,
    B_LS_LEFT, B_LS_UP, B_LS_RIGHT, B_LS_DOWN, B_RS_LEFT, B_RS_UP, B_RS_RIGHT, B_RS_DOWN,
};
#define BIT(n) (1ull << (n))
#define STICK_MAX 32767
#define STICK_DIR 16384   // the console sets the stick direction bits past half way

static PlatMutex lock = PLAT_MUTEX_INIT;
static int initialized;
static uint32_t supported_styles = STYLE_FULL_KEY;

// ---- XInput, loaded at run time
typedef struct { uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; } XPad;
typedef struct { uint32_t packet; XPad pad; } XState;
typedef struct { uint16_t left, right; } XVibration;
#ifdef _WIN32
static DWORD(WINAPI *x_get)(DWORD, XState *);
static DWORD(WINAPI *x_set)(DWORD, XVibration *);
#else
#define ERROR_SUCCESS 0
static uint32_t (*x_get)(uint32_t, XState *);   // no XInput: stays NULL
static uint32_t (*x_set)(uint32_t, XVibration *);
typedef uint32_t DWORD;
#endif
static PlatOnce xinput_once = PLAT_ONCE_INIT;

static void xinput_load(void *arg) {
    (void)arg;
#ifdef _WIN32
    HMODULE m = LoadLibraryA("xinput1_4.dll");
    if (!m) m = LoadLibraryA("xinput9_1_0.dll");
    if (m) {
        x_get = (DWORD(WINAPI *)(DWORD, XState *))(void *)GetProcAddress(m, "XInputGetState");
        x_set = (DWORD(WINAPI *)(DWORD, XVibration *))(void *)GetProcAddress(m, "XInputSetState");
    }
#endif
    rt_log("hid: XInput %s; keyboard while the game window has focus", x_get ? "loaded" : "not available");
}

static int pad_slot = -1;   // the XInput user index read last, -1 none

static int xinput_read(XPad *out) {
    plat_once(&xinput_once, xinput_load, NULL);
    if (!x_get) return 0;
    XState s;
    if (pad_slot >= 0 && x_get((DWORD)pad_slot, &s) == ERROR_SUCCESS) {
        *out = s.pad;
        return 1;
    }
    for (int i = 0; i < 4; i++)
        if (x_get((DWORD)i, &s) == ERROR_SUCCESS) {
            if (pad_slot != i) rt_log("hid: XInput pad %d", i);
            pad_slot = i;
            *out = s.pad;
            return 1;
        }
    pad_slot = -1;
    return 0;
}

static int32_t clamp_stick(int32_t v) { return v > STICK_MAX ? STICK_MAX : v < -STICK_MAX ? -STICK_MAX : v; }

// ---- the settings this file reads, again whenever cfg_version changes (the settings menu applies them live)
static unsigned knobs_ver;
static double dz_l, dz_r, rumble_gain = 1;
static int rumble_on;   // MLB_RUMBLE: read here, not per call (cfg_get is two getenv calls and a table scan)

static double dead_zone_of(const char *key) {
    const char *v = cfg_get(key);   // unset or empty: both sticks' value
    double d = v && *v ? cfg_double(key) : cfg_double("STICK_DEADZONE");
    return d < 0 ? 0 : d > 50 ? 0.5 : d / 100;
}

static void knobs(void) {
    unsigned v = cfg_version();
    if (v == __atomic_load_n(&knobs_ver, __ATOMIC_ACQUIRE)) return;
    dz_l = dead_zone_of("LSTICK_DEADZONE");
    dz_r = dead_zone_of("RSTICK_DEADZONE");
    double g = cfg_double("RUMBLE_STRENGTH");
    rumble_gain = g < 0 ? 0 : g / 100;
    rumble_on = cfg_bool("MLB_RUMBLE");
    __atomic_store_n(&knobs_ver, v, __ATOMIC_RELEASE);
}

// A radial dead zone: a stick inside dz of its travel reads as centred; past it, the rest of the travel is spread
// over the whole range, so a small push still starts at zero and full deflection stays full.
static void dead_zone(int32_t *x, int32_t *y, double dz) {
    if (dz <= 0) return;
    double fx = *x / (double)STICK_MAX, fy = *y / (double)STICK_MAX, m = sqrt(fx * fx + fy * fy);
    if (m <= dz) {
        *x = *y = 0;
        return;
    }
    double out = (m - dz) / (1 - dz);
    double s = (out > 1 ? 1 : out) / m;
    *x = clamp_stick((int32_t)lrint(fx * s * STICK_MAX));
    *y = clamp_stick((int32_t)lrint(fy * s * STICK_MAX));
}

static int key(int vk) { return wsys_key_down(vk); }   // after one wsys_focused check (poll)

// ---- face buttons (MLB_PAD_MAP)
// The pads' face buttons in place order: south, east, west, north. By position they are Switch B, A, Y, X, where
// the console has them; by label each is the Switch button of its letter (FaceIn.lab).
static const int face_pos[4] = {B_B, B_A, B_Y, B_X};
typedef struct { uint8_t held; uint64_t lab[4]; } FaceIn;   // held: bit f for place f; lab[f]: Switch bits by label

// fastball: MLB_PAD_MAP=auto follows the game's own log. Its UI engine logs every screen it enters
// ("uiEngine: Entering screen: [<movie>-><screen> (n)]", lm). Two recorded sessions (menus, an exhibition, Road to
// the Show practice, the pause menu, batting, pitching, fielding and its slow-motion throw) show menu movies entered
// for every menu, the in-game pause menu included, and the play HUD movies entered again whenever play resumes. So
// the last such movie entered says whether a menu or play has the controller; the stadium boards, presentation
// graphics and notifications the game also enters say nothing and change nothing.
static volatile int screen_play;   // 1 after a play screen, 0 after a menu screen (and at boot)

static int movie_is(const char *m, int n, const char *const *list) {
    for (; *list; list++)
        if ((int)strlen(*list) == n && !memcmp(m, *list, (size_t)n)) return 1;
    return 0;
}

void hid_game_log(const char *text, int n) {
    static const char pre[] = "uiEngine: Entering screen: [";
    enum { PRE = sizeof pre - 1 };
    if (n <= PRE || memcmp(text, pre, PRE)) return;
    const char *m = text + PRE, *end = text + n, *arrow = m;
    while (arrow + 1 < end && !(arrow[0] == '-' && arrow[1] == '>')) arrow++;
    if (arrow + 1 >= end) return;
    int mn = (int)(arrow - m);
    static const char *const menus[] = {
        "TitleLoadingScreen", "PM_Main_Alternate", "Pause_Menu", "Selection Box 2", "SubMenu Selection Box 2", "Dialog",
        "Keyboard", "CareerDecision", "LaunchGame", "Topbar", "Background", NULL,
    };
    static const char *const play[] = {
        "Scorebar", "TM_ScoreBar", "PitchSelect_Bars", "PitchHistory", "PitchMeter_Feedback", "SwingAnalysis",
        "Fielding_Assets", "Fielding_Assistance", "Bullet_Time_Assets", "Lead_Runner", NULL,
    };
    int now;
    if (movie_is(m, mn, play)) now = 1;
    else if (movie_is(m, mn, menus) || (mn > 3 && !memcmp(m, "ol_", 3))) now = 0;   // ol_: the online and tutorial popups
    else return;
    if (__atomic_exchange_n(&screen_play, now, __ATOMIC_RELAXED) != now &&
        !strcmp(cfg_value("MLB_PAD_MAP"), "auto"))
        rt_log("hid: face buttons by %s (screen %.*s)", now ? "position, as on PlayStation and Xbox" : "label",
               (int)(end - m - 1 > 0 ? end - m - 1 : 0), m);
}

int hid_face_position(void) {
    static unsigned seen = ~0u;
    static int mode;   // 0 label, 1 position, 2 auto
    unsigned v = cfg_version();
    if (v != seen) {
        const char *m = cfg_value("MLB_PAD_MAP");
        mode = !strcmp(m, "position") ? 1 : !strcmp(m, "label") ? 0 : 2;
        seen = v;
    }
    return mode == 2 ? __atomic_load_n(&screen_play, __ATOMIC_RELAXED) : mode;
}

// The Switch bits of the held face buttons. When the mapping changes (auto: a screen changed), a button held at that
// moment is ignored until released: the A that leaves the pause menu would otherwise turn into Switch B, a throw home
// or a swing, the moment play resumes. overlay: the settings menu's own read, always by label and outside the latch.
static uint64_t face_bits(const FaceIn *f, int overlay) {
    static PlatMutex face_lock = PLAT_MUTEX_INIT;
    static int last = -1;
    static uint8_t blocked;
    int pos = overlay ? 0 : hid_face_position();
    uint8_t use = f->held;
    if (!overlay) {
        plat_mutex_lock(&face_lock);
        if (pos != last) blocked = last < 0 ? 0 : f->held, last = pos;
        blocked &= f->held;
        use &= (uint8_t)~blocked;
        plat_mutex_unlock(&face_lock);
    }
    uint64_t b = 0;
    for (int i = 0; i < 4; i++)
        if (use >> i & 1) b |= pos ? BIT(face_pos[i]) : f->lab[i];
    return b;
}

// The Switch buttons of an XInput pad state, but the face buttons (into *face).
static uint64_t pad_bits(const XPad *p, FaceIn *face) {
    uint64_t b = 0;
    static const struct { uint16_t x; int lab; } faces[4] = {   // Xbox A B X Y: south, east, west, north
        {0x1000, B_A}, {0x2000, B_B}, {0x4000, B_X}, {0x8000, B_Y},
    };
    for (int i = 0; i < 4; i++)
        if (p->buttons & faces[i].x) face->held |= (uint8_t)(1u << i), face->lab[i] |= BIT(faces[i].lab);
    static const struct { uint16_t x; int bit; } other[] = {
        {0x0001, B_UP}, {0x0002, B_DOWN}, {0x0004, B_LEFT}, {0x0008, B_RIGHT}, {0x0010, B_PLUS}, {0x0020, B_MINUS},
        {0x0040, B_LSTICK}, {0x0080, B_RSTICK}, {0x0100, B_L}, {0x0200, B_R},
    };
    for (size_t i = 0; i < sizeof other / sizeof *other; i++)
        if (p->buttons & other[i].x) b |= BIT(other[i].bit);
    if (p->lt > 127) b |= BIT(B_ZL);
    if (p->rt > 127) b |= BIT(B_ZR);
    return b;
}

// ---- SDL3, loaded at run time from mlb.exe's folder. Every SDL call is made under sdl_lock: the game's npad reads,
// the settings menu (render thread) and vibration come from different threads.
#define SDL_FNS(X)                                                                                                     \
    X(SDL_SetHint) X(SDL_Init) X(SDL_GetError) X(SDL_GetVersion) X(SDL_SetEventEnabled) X(SDL_free)                  \
    X(SDL_GetGamepads) X(SDL_OpenGamepad) X(SDL_CloseGamepad) X(SDL_GamepadConnected) X(SDL_UpdateGamepads)          \
    X(SDL_GetGamepadButton) X(SDL_GetGamepadAxis) X(SDL_GetGamepadButtonLabel) X(SDL_GetGamepadName)                 \
    X(SDL_GetGamepadType) X(SDL_GetGamepadProperties) X(SDL_GetBooleanProperty) X(SDL_RumbleGamepad)
#define SDL_PTR(f) static __typeof__(f) *p_##f;
SDL_FNS(SDL_PTR)
#undef SDL_PTR

#define SCAN_NS 1000000000ull   // a new controller is looked for once a second
#define UPDATE_NS 2000000ull    // SDL reads the controllers at most every 2 ms (the console samples every 5)
enum { MAX_PADS = 4 };
typedef struct {
    SDL_Gamepad *g;
    SDL_JoystickID id;
    int face_label[4];   // the Switch button of the south, east, west, north button by its label, -1 unknown
    int kind;            // fastball: for the button icons (prompts.h hid_prompt_kind): 1 Xbox, 2 PlayStation, 0 other
} SdlPad;
static PlatMutex sdl_lock = PLAT_MUTEX_INIT;
static PlatOnce sdl_once = PLAT_ONCE_INIT;
static int sdl_ok;   // SDL3 loaded and its gamepad subsystem up: the pads come from SDL, not XInput
static SdlPad pads[MAX_PADS];
static int npads;
static uint64_t next_scan, last_update;

static const char *pad_family(SDL_GamepadType t) {
    switch (t) {
    case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: return "Xbox";
    case SDL_GAMEPAD_TYPE_PS3: return "PlayStation 3";
    case SDL_GAMEPAD_TYPE_PS4: return "PlayStation 4";
    case SDL_GAMEPAD_TYPE_PS5: return "PlayStation 5";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Nintendo";
    default: return "generic";
    }
}

static void sdl_load(void *arg) {
    (void)arg;
    if (!strcmp(cfg_value("MLB_PAD_DRIVER"), "xinput")) {
        rt_log("hid: MLB_PAD_DRIVER=xinput: Xbox pads through XInput only");
        return;
    }
#ifdef _WIN32
    wchar_t path[MAX_PATH + 16];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    while (n && path[n - 1] != L'\\' && path[n - 1] != L'/') n--;
    if (!n || n >= MAX_PATH) return;
    wcscpy(path + n, L"SDL3.dll");   // this folder only: no other SDL3.dll on the search path gets loaded
    HMODULE m = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        rt_log("hid: no SDL3.dll next to mlb.exe (tools/build.py copies it): Xbox pads through XInput only, no "
               "PlayStation pads");
        return;
    }
    int all = 1;
#define SDL_GET(f) all &= (p_##f = (__typeof__(f) *)(void *)GetProcAddress(m, #f)) != NULL;
    SDL_FNS(SDL_GET)
#undef SDL_GET
    if (!all) {
        rt_log("hid: SDL3.dll lacks a function this port needs: XInput only");
        return;
    }
#else
#define SDL_GET(f) p_##f = f;
    SDL_FNS(SDL_GET)
#undef SDL_GET
#endif
    // No SDL window: the pads count whichever window is in front, as XInput did (the keyboard keeps its focus check).
    p_SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!p_SDL_Init(SDL_INIT_GAMEPAD)) {
        rt_log("hid: SDL gamepad init failed (%s): XInput only", p_SDL_GetError());
        return;
    }
    // Nothing drains SDL's event queue: the per-input events stay off (the state still updates).
    static const SDL_EventType quiet[] = {
        SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_JOYSTICK_BALL_MOTION, SDL_EVENT_JOYSTICK_HAT_MOTION,
        SDL_EVENT_JOYSTICK_BUTTON_DOWN, SDL_EVENT_JOYSTICK_BUTTON_UP, SDL_EVENT_JOYSTICK_BATTERY_UPDATED,
        SDL_EVENT_JOYSTICK_UPDATE_COMPLETE, SDL_EVENT_GAMEPAD_AXIS_MOTION, SDL_EVENT_GAMEPAD_BUTTON_DOWN,
        SDL_EVENT_GAMEPAD_BUTTON_UP, SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN, SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION,
        SDL_EVENT_GAMEPAD_TOUCHPAD_UP, SDL_EVENT_GAMEPAD_SENSOR_UPDATE, SDL_EVENT_GAMEPAD_UPDATE_COMPLETE,
    };
    for (size_t i = 0; i < sizeof quiet / sizeof *quiet; i++) p_SDL_SetEventEnabled(quiet[i], false);
    int v = p_SDL_GetVersion();
    rt_log("hid: SDL %d.%d.%d: Xbox and PlayStation controllers; keyboard while the game window has focus",
           SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    sdl_ok = 1;
}

static int switch_of_label(SDL_GamepadButtonLabel l) {
    switch (l) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return B_A;
    case SDL_GAMEPAD_BUTTON_LABEL_B: case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return B_B;
    case SDL_GAMEPAD_BUTTON_LABEL_X: case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return B_X;
    case SDL_GAMEPAD_BUTTON_LABEL_Y: case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return B_Y;
    default: return -1;
    }
}

static const SDL_GamepadButton face_sdl[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                              SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH};

// fastball: the kind of controller whose buttons the game's prompts show (prompts.c, MLB_PROMPTS=auto): the one
// used last (a button pressed or a stick pushed past half way), else the one connected first; 0 without a controller.
// A pad of a type SDL does not name goes by its south button's label (A: Xbox-like, cross: PlayStation); Nintendo pads
// keep the game's own icons.
static volatile int prompt_kind;
int hid_prompt_kind(void) { return __atomic_load_n(&prompt_kind, __ATOMIC_RELAXED); }

static void prompt_kind_set(int kind) {
    if (__atomic_exchange_n(&prompt_kind, kind, __ATOMIC_RELAXED) != kind)
        rt_log("hid: button icons for %s", kind == 1 ? "Xbox" : kind == 2 ? "PlayStation" : "Nintendo (the game's own)");
}

static int pad_kind(SDL_Gamepad *g) {
    switch (p_SDL_GetGamepadType(g)) {
    case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: return 1;
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return 2;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return 0;
    default: break;
    }
    SDL_GamepadButtonLabel l = p_SDL_GetGamepadButtonLabel(g, SDL_GAMEPAD_BUTTON_SOUTH);
    return l == SDL_GAMEPAD_BUTTON_LABEL_A ? 1 : l == SDL_GAMEPAD_BUTTON_LABEL_CROSS ? 2 : 0;
}

// Under sdl_lock: drop the pads that went away; once a second open the ones that arrived. Logs each change.
static void sdl_scan(void) {
    for (int i = 0; i < npads;)
        if (p_SDL_GamepadConnected(pads[i].g)) i++;
        else {
            rt_log("hid: controller %u disconnected", (unsigned)pads[i].id);
            p_SDL_CloseGamepad(pads[i].g);
            pads[i] = pads[--npads];
            prompt_kind_set(npads ? pads[0].kind : 0);   // fastball: a pad still there, until another is used
        }
    uint64_t now = plat_now_ns();
    if (now < next_scan) return;
    next_scan = now + SCAN_NS;
    int n = 0;
    SDL_JoystickID *ids = p_SDL_GetGamepads(&n);
    for (int k = 0; ids && k < n && npads < MAX_PADS; k++) {
        int have = 0;
        for (int i = 0; i < npads; i++) have |= pads[i].id == ids[k];
        if (have) continue;
        SDL_Gamepad *g = p_SDL_OpenGamepad(ids[k]);
        if (!g) continue;
        SdlPad *p = &pads[npads++];
        p->g = g, p->id = ids[k];
        for (int f = 0; f < 4; f++) p->face_label[f] = switch_of_label(p_SDL_GetGamepadButtonLabel(g, face_sdl[f]));
        p->kind = pad_kind(g);
        if (npads == 1) prompt_kind_set(p->kind);   // fastball: the first pad, until another is used
        int rumble = p_SDL_GetBooleanProperty(p_SDL_GetGamepadProperties(g), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        const char *name = p_SDL_GetGamepadName(g);
        rt_log("hid: controller %u \"%s\" connected (%s layout, %s)", (unsigned)p->id, name ? name : "?",
               pad_family(p_SDL_GetGamepadType(g)), rumble ? "rumble" : "no rumble");
    }
    p_SDL_free(ids);
}

// Under sdl_lock: the Switch buttons of one SDL pad but the face buttons (into *face); its guide button (Xbox, PS)
// ORs into *guide.
static uint64_t sdl_bits(const SdlPad *p, FaceIn *face, int *guide) {
    SDL_Gamepad *g = p->g;
    uint64_t b = 0;
    // By label: the letter or PlayStation symbol on the button; a pad without known labels as an Xbox pad (south A,
    // east B, west X, north Y). By position: face_pos.
    static const int xbox[4] = {B_A, B_B, B_X, B_Y};
    for (int f = 0; f < 4; f++)
        if (p_SDL_GetGamepadButton(g, face_sdl[f]))
            face->held |= (uint8_t)(1u << f), face->lab[f] |= BIT(p->face_label[f] >= 0 ? p->face_label[f] : xbox[f]);
    static const struct { SDL_GamepadButton s; int bit; } other[] = {
        {SDL_GAMEPAD_BUTTON_DPAD_UP, B_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, B_DOWN},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, B_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, B_RIGHT},
        {SDL_GAMEPAD_BUTTON_START, B_PLUS}, {SDL_GAMEPAD_BUTTON_BACK, B_MINUS}, {SDL_GAMEPAD_BUTTON_TOUCHPAD, B_MINUS},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, B_LSTICK}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, B_RSTICK},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, B_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, B_R},
    };
    for (size_t i = 0; i < sizeof other / sizeof *other; i++)
        if (p_SDL_GetGamepadButton(g, other[i].s)) b |= BIT(other[i].bit);
    if (p_SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384) b |= BIT(B_ZL);
    if (p_SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384) b |= BIT(B_ZR);
    *guide |= p_SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_GUIDE);
    return b;
}

// ---- every controller merged: buttons ORed, each stick from the pad pushed furthest (raw, before the dead zone)
typedef struct { uint64_t buttons; int32_t lx, ly, rx, ry; int guide; } PadIn;

static void take_stick(int32_t *x, int32_t *y, int32_t nx, int32_t ny) {
    if ((int64_t)nx * nx + (int64_t)ny * ny > (int64_t)*x * *x + (int64_t)*y * *y) *x = nx, *y = ny;
}

// 1 when a controller is connected. SDL's y axes grow downwards, the Switch's upwards. overlay: the settings menu's
// read (face_bits).
static int pad_read(PadIn *in, int overlay) {
    memset(in, 0, sizeof *in);
    FaceIn face = {0};
    plat_once(&sdl_once, sdl_load, NULL);
    if (!sdl_ok) {
        XPad p;
        if (!xinput_read(&p)) {
            if (prompt_kind) prompt_kind_set(0);
            return 0;
        }
        if (prompt_kind != 1) prompt_kind_set(1);   // fastball: XInput pads are Xbox pads (button icons)
        in->buttons = pad_bits(&p, &face) | face_bits(&face, overlay);
        in->lx = clamp_stick(p.lx), in->ly = clamp_stick(p.ly), in->rx = clamp_stick(p.rx), in->ry = clamp_stick(p.ry);
        return 1;
    }
    plat_mutex_lock(&sdl_lock);
    uint64_t now = plat_now_ns();
    if (now - last_update >= UPDATE_NS) {   // the game reads the npad several times a frame: one update serves them
        last_update = now;
        p_SDL_UpdateGamepads();
        sdl_scan();
    }
    for (int i = 0; i < npads; i++) {
        SDL_Gamepad *g = pads[i].g;
        FaceIn pf = {0};
        uint64_t b = sdl_bits(&pads[i], &pf, &in->guide);
        face.held |= pf.held;
        for (int f = 0; f < 4; f++) face.lab[f] |= pf.lab[f];
        int32_t lx = clamp_stick(p_SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX));
        int32_t ly = clamp_stick(-(int32_t)p_SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
        int32_t rx = clamp_stick(p_SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX));
        int32_t ry = clamp_stick(-(int32_t)p_SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
        in->buttons |= b;
        take_stick(&in->lx, &in->ly, lx, ly);
        take_stick(&in->rx, &in->ry, rx, ry);
        // fastball: this pad is the one in use (button icons, prompt_kind)
        if (npads > 1 && pads[i].kind != prompt_kind &&
            (b || pf.held || abs(lx) > STICK_DIR || abs(ly) > STICK_DIR || abs(rx) > STICK_DIR || abs(ry) > STICK_DIR))
            prompt_kind_set(pads[i].kind);
    }
    int have = npads > 0;
    plat_mutex_unlock(&sdl_lock);
    in->buttons |= face_bits(&face, overlay);
    return have;
}

// Both motors of every controller, 0..65535 each.
static void pad_rumble(uint16_t lo, uint16_t hi) {
    plat_once(&sdl_once, sdl_load, NULL);
    if (sdl_ok) {
        plat_mutex_lock(&sdl_lock);
        // SDL stops a rumble after its duration: the longest it takes, as XInput's motors run until told otherwise.
        // The same values again only extend it (SDL sends no report then).
        for (int i = 0; i < npads; i++) p_SDL_RumbleGamepad(pads[i].g, lo, hi, 0xFFFF);
        plat_mutex_unlock(&sdl_lock);
        return;
    }
    plat_once(&xinput_once, xinput_load, NULL);
    if (x_set && pad_slot >= 0) {
        XVibration xv = {lo, hi};
        x_set((DWORD)pad_slot, &xv);
    }
}

// fastball: the in-game settings menu (ui/menu.cpp) reads the pad itself while it is open, in Switch button bits
// (the same as the game's: A 0, B 1, X 2, Y 3, L 6, R 7, ZL 8, ZR 9, d-pad 12..15). 0 without a pad.
int hid_menu_pad(uint64_t *buttons, int32_t *lx, int32_t *ly) {
    PadIn in;
    int have = pad_read(&in, 1);
    *buttons = in.buttons, *lx = in.lx, *ly = in.ly;
    return have;
}

// fastball: the settings menu's dead zone rows show how far each stick is pushed now, before the dead zone (a
// drifting stick's resting value). now[0] left, now[1] right, in percent of full travel; 0 without a pad.
int hid_stick_percent(double now[2]) {
    PadIn in;
    now[0] = now[1] = 0;
    if (!pad_read(&in, 1)) return 0;
    double lx = in.lx / (double)STICK_MAX, ly = in.ly / (double)STICK_MAX, rx = in.rx / (double)STICK_MAX,
           ry = in.ry / (double)STICK_MAX;
    now[0] = fmin(sqrt(lx * lx + ly * ly), 1) * 100;
    now[1] = fmin(sqrt(rx * rx + ry * ry), 1) * 100;
    return 1;
}

// fastball: the software keyboard's window is open (swkbd.c): the game reads an idle controller (poll).
static volatile int text_entry_open;
void input_block(int on) { __atomic_store_n(&text_entry_open, on ? 1 : 0, __ATOMIC_RELEASE); }

// The merged controller state: buttons and sticks.
static void poll(NpadState *st) {
    uint64_t b = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    int guide = 0;
    PadIn in;
    knobs();
    if (pad_read(&in, 0)) {
        b = in.buttons, guide = in.guide;
        lx = in.lx, ly = in.ly, rx = in.rx, ry = in.ry;
        dead_zone(&lx, &ly, dz_l);
        dead_zone(&rx, &ry, dz_r);
    }
    if (wsys_focused()) {
        static const struct { int vk, bit; } keys[] = {
            {KEY_RETURN, B_A}, {KEY_SPACE, B_A}, {KEY_ESCAPE, B_B}, {KEY_BACK, B_B}, {'R', B_X}, {'F', B_Y}, {'Q', B_L},
            {'E', B_R}, {'1', B_ZL}, {'3', B_ZR}, {KEY_TAB, B_PLUS}, {'M', B_MINUS}, {'C', B_LSTICK}, {'V', B_RSTICK},
            {KEY_LEFT, B_LEFT}, {KEY_UP, B_UP}, {KEY_RIGHT, B_RIGHT}, {KEY_DOWN, B_DOWN},
        };
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
            if (key(keys[i].vk)) b |= BIT(keys[i].bit);
        int kx = key('D') - key('A'), ky = key('W') - key('S');
        int jx = key('L') - key('J'), jy = key('I') - key('K');
        if (kx || ky) lx = kx * STICK_MAX, ly = ky * STICK_MAX;
        if (jx || jy) rx = jx * STICK_MAX, ry = jy * STICK_MAX;
    }
    if (lx <= -STICK_DIR) b |= BIT(B_LS_LEFT);
    if (lx >= STICK_DIR) b |= BIT(B_LS_RIGHT);
    if (ly >= STICK_DIR) b |= BIT(B_LS_UP);
    if (ly <= -STICK_DIR) b |= BIT(B_LS_DOWN);
    if (rx <= -STICK_DIR) b |= BIT(B_RS_LEFT);
    if (rx >= STICK_DIR) b |= BIT(B_RS_RIGHT);
    if (ry >= STICK_DIR) b |= BIT(B_RS_UP);
    if (ry <= -STICK_DIR) b |= BIT(B_RS_DOWN);
    // fastball: the in-game settings menu (ui/menu.cpp). Minus and Plus pressed together (Back and Start on an
    // Xbox pad, Share / Create and Options on a PlayStation pad) or the guide button (Xbox, PS) open or close it; while
    // it is open the game gets an idle controller. Buttons held when it closes stay hidden from the game until
    // released: the B that closed the menu would otherwise also go "back" in the game.
    // The software keyboard's window (swkbd.c, input_block) blocks the game's controller the same way, so the Enter or
    // A that confirmed the text does not also press A in the game.
    static uint64_t prev, hidden;
    static int was_open, prev_guide;
    const uint64_t combo = BIT(B_MINUS) | BIT(B_PLUS);
    int typing = __atomic_load_n(&text_entry_open, __ATOMIC_ACQUIRE);
    if (!typing && (((b & combo) == combo && (prev & combo) != combo) || (guide && !prev_guide))) menu_toggle();
    prev = b, prev_guide = guide;
    int open = menu_is_open() || typing;
    if (open) hidden = ~0ull;
    else if (was_open) hidden = b;
    was_open = open;
    hidden &= b;   // a released button counts again
    b &= ~hidden;
    if (open) lx = ly = rx = ry = 0;
    st->buttons = b;
    st->lx = lx, st->ly = ly, st->rx = rx, st->ry = ry;
}

static uint32_t id_of(Cpu *c, int arg, const char *fn) {
    const uint32_t *id = arg_p(c, arg);
    if (!id) rt_fatal("nn::hid::%s: null NpadId", fn);
    return *id;
}

static int connected(uint32_t id) {
    plat_mutex_lock_shared(&lock);
    int on = initialized && id == ID_NO1 && (supported_styles & STYLE_FULL_KEY);
    plat_mutex_unlock_shared(&lock);
    return on;
}

// ---- overrides
// void InitializeNpad()
static void ov_initialize_npad(Cpu *c) {
    (void)c;
    plat_mutex_lock(&lock);
    initialized = 1;
    plat_mutex_unlock(&lock);
    rt_log("hid: one full-key controller as player 1");
}

// void SetSupportedNpadIdType(const NpadIdType *ids, size_t count)
static void ov_supported_ids(Cpu *c) {
    const uint32_t *ids = arg_p(c, 0);
    uint64_t n = arg_x(c, 1);
    if (n > 10 || (!ids && n)) rt_fatal("nn::hid::SetSupportedNpadIdType: %llu ids", (unsigned long long)n);
    int no1 = 0;
    for (uint64_t i = 0; i < n; i++) no1 |= ids[i] == ID_NO1;
    if (!no1) rt_log("hid: the game does not list NpadId No1 as supported; the controller stays player 1");
}

// void SetSupportedNpadStyleSet(NpadStyleSet styles): a 32-bit flag set in w0
static void ov_set_supported_styles(Cpu *c) {
    plat_mutex_lock(&lock);
    supported_styles = (uint32_t)arg_x(c, 0);
    plat_mutex_unlock(&lock);
    if (!(supported_styles & STYLE_FULL_KEY))
        rt_log("hid: the game does not support the full-key style (%#x): the controller reads as disconnected",
               supported_styles);
}

// NpadStyleSet GetSupportedNpadStyleSet()
static void ov_get_supported_styles(Cpu *c) {
    plat_mutex_lock_shared(&lock);
    ret_x(c, supported_styles);
    plat_mutex_unlock_shared(&lock);
}

// NpadStyleSet GetNpadStyleSet(const NpadIdType &id)
static void ov_style_set(Cpu *c) { ret_x(c, connected(id_of(c, 0, "GetNpadStyleSet")) ? STYLE_FULL_KEY : 0); }

// fastball: held-button repeat at 60 ticks a second whatever the frame rate (MLB_FPS_LIMIT, framerate.c).
// MLB's pad update (1.0.20 f_7001341c20, 1.0.15 f_7001340720; read 2026-11-17) repeats a held d-pad or stick
// direction by counting frames, not time: each pad record (0x790 bytes, an array whose address the game keeps at
// 1.0.20 main+0x45aef28, 1.0.15 main+0x45acf28; 7 records seen) holds
//   +0x1a4 the initial delay (20 frames as shipped)  +0x1a8 its countdown
//   +0x1ac the repeat interval (2 frames)              +0x1b4 its countdown
// Every update of a pad with a repeatable button held takes one off the delay countdown, then one off the interval
// countdown, and repeats the button when the latter is at 0 (then back to the interval); with none held both go
// back to their starting values. At 60 fps: a repeat after a third of a second, then 20 a second. Unlocked, the
// countdowns ran with the frame rate: at 240 fps a held key repeated after 80 ms, then 80 times a second.
// The game reads the controller (this file's GetNpadState) right before it updates its pads, on the same thread. Each
// read here lets a pad's countdowns move by one only when 1/60 s of time is available for it, and puts back a step
// the game took without it (a step to 0 that already fired a repeat stays). Below 60 fps the countdowns get the
// missing steps here. At the game's own 60 nothing here changes.
enum { PAD_SIZE = 0x790, PAD_MAX = 8, PAD_DELAY = 0x1a4, PAD_DELAY_N = 0x1a8, PAD_RATE = 0x1ac, PAD_RATE_N = 0x1b4 };

static void repeat_pace(uint64_t now) {
    static const struct { const char *build; uint64_t slot; } k_slot[] = {{"1.0.15", 0x70045acf28ull},
                                                                         {"1.0.20", 0x70045aef28ull}};
    static uint64_t slot, last_ns;
    static int slot_read;
    static struct { uint32_t d, r; double credit; int have; } st[PAD_MAX];
    if (!slot_read) {
        slot_read = 1;
        for (size_t i = 0; i < sizeof k_slot / sizeof *k_slot; i++)
            if (!strcmp(rt_game_build, k_slot[i].build)) slot = k_slot[i].slot;
    }
    double ticks = last_ns && now > last_ns ? (double)(now - last_ns) * 60 / 1e9 : 0;
    last_ns = now;
    if (ticks > 3) ticks = 3;   // a load or a pause: no burst of steps after it
    int on = slot && frame_step_realtime();
    uint64_t base = slot ? *(volatile uint64_t *)(uintptr_t)slot : 0;
    if (!on || !base || !mem_guest_readable(base, (uint64_t)PAD_SIZE * PAD_MAX)) {
        for (int i = 0; i < PAD_MAX; i++) st[i].have = 0;
        return;
    }
    for (int i = 0; i < PAD_MAX; i++) {
        volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)(base + (uint64_t)i * PAD_SIZE);
        uint32_t delay = p[PAD_DELAY / 4], rate = p[PAD_RATE / 4];
        if (!delay || delay > 600 || !rate || rate > 600) break;   // past the game's records
        uint32_t d = p[PAD_DELAY_N / 4], r = p[PAD_RATE_N / 4];
        if (!st[i].have || (d == delay && r == rate)) {   // first look, or nothing held: the game's start values
            st[i].d = d, st[i].r = r, st[i].credit = 1, st[i].have = 1;
            continue;
        }
        double credit = st[i].credit + ticks;
        if (credit > 3) credit = 3;
        int stepped = d != st[i].d || r != st[i].r;
        int fired = st[i].d == 0 && d == 0 && st[i].r == 0;   // the game repeated the button: that step stays
        if (stepped && (fired || credit >= 1)) credit -= 1;
        else if (stepped) d = st[i].d, r = st[i].r;          // too soon: the step is undone
        while (credit >= 2) {                                 // below 60 fps: the steps the frames missed
            if (d > 1) d--;
            else if (d == 0 && r > 1) r--;
            else break;
            credit -= 1;
        }
        p[PAD_DELAY_N / 4] = d, p[PAD_RATE_N / 4] = r;
        st[i].d = d, st[i].r = r, st[i].credit = credit;
    }
}

// void GetNpadState(NpadXxxState *out, const NpadIdType &id): the latest sample. The full-key state is the
// controller's; the other styles' states read as disconnected (all zero but the sampling number).
static void npad_state(Cpu *c, int full_key, const char *fn) {
    NpadState *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::hid::%s: null state", fn);
    uint32_t id = id_of(c, 1, fn);
    NpadState st;
    memset(&st, 0, sizeof st);
    uint64_t now = kern_now_ns();
    st.sampling = now / 5000000;   // the console samples every 5 ms
    if (full_key && connected(id)) {
        // fastball: one reading serves the calls of the next 1 ms. The game asks several times a frame on its main
        // thread, and each reading asks SDL (or XInput) for the pads, Windows for 26 keys and the foreground window:
        // 2% of that thread's time in the attract demo match at an unlocked frame rate. The console samples every 5 ms.
        static PlatMutex poll_lock = PLAT_MUTEX_INIT;
        static NpadState last;
        static uint64_t last_ns;
        plat_mutex_lock(&poll_lock);
        if (!last_ns || now - last_ns >= 1000000) poll(&last), last_ns = now;
        st.buttons = last.buttons;
        st.lx = last.lx, st.ly = last.ly, st.rx = last.rx, st.ry = last.ry;
        repeat_pace(now);
        plat_mutex_unlock(&poll_lock);
        st.attributes = ATTR_CONNECTED | ATTR_WIRED;
    }
    *out = st;
}
static void ov_state_full_key(Cpu *c) { npad_state(c, 1, "GetNpadState(FullKey)"); }
static void ov_state_other(Cpu *c) { npad_state(c, 0, "GetNpadState"); }

// void SetNpadJoyHoldType(NpadJoyHoldType): only matters for single Joy-Cons
static void ov_hold_type(Cpu *c) { (void)c; }

// void InitializeTouchScreen(): no touch input on this host
static void ov_initialize_touch(Cpu *c) { (void)c; }

// int GetTouchScreenStates<1>(TouchScreenState<1> *out, int count): one sample with no touches. TouchScreenState<1> is
// {u64 sampling number, s32 touch count, u32 reserved, TouchState[1] (0x28 bytes)} (libnx HidTouchScreenState).
static void ov_touch_states(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    int n = (int)arg_x(c, 1);
    if (!out || n < 1) { ret_x(c, 0); return; }
    memset(out, 0, 0x38);
    uint64_t sampling = kern_now_ns() / 5000000;
    memcpy(out, &sampling, 8);
    ret_x(c, 1);
}

// Result ShowControllerSupport(ControllerSupportResultInfo *out, const ControllerSupportArg &): the applet would let
// the player pick controllers; one is always connected, so it returns at once with player 1 (TotK ov_input.c)
static void ov_controller_support(Cpu *c) {
    ControllerSupportResult *out = arg_p(c, 0);
    if (out) *out = (ControllerSupportResult){1, {0}, ID_NO1};
    rt_log("hid: controller support applet answered with one controller");
    ret_x(c, 0);
}

// ---- vibration: two devices (left, right motor) on the controller. A handle is {type, player, device, pad}.
enum { VIB_TYPE_FULL_KEY = 3 };
static float vib_low, vib_high;

// int GetVibrationDeviceHandles(VibrationDeviceHandle *out, int n, const NpadIdType &id, NpadStyleSet style)
static void ov_vib_handles(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    int n = (int)arg_x(c, 1);
    uint32_t id = id_of(c, 2, "GetVibrationDeviceHandles");
    int k = 0;
    for (; out && k < n && k < 2; k++) {
        uint8_t h[4] = {VIB_TYPE_FULL_KEY, (uint8_t)id, (uint8_t)k, 0};
        memcpy(out + 4 * k, h, 4);
    }
    ret_x(c, (uint64_t)k);
}

// void InitializeVibrationDevice(const VibrationDeviceHandle &)
static void ov_vib_init(Cpu *c) { (void)c; }

// void GetVibrationDeviceInfo(VibrationDeviceInfo *out, const VibrationDeviceHandle &): linear resonant actuators,
// left and right
static void ov_vib_info(Cpu *c) {
    VibrationDeviceInfo *out = arg_p(c, 0);
    const uint8_t *h = arg_p(c, 1);
    if (out && h) *out = (VibrationDeviceInfo){1, h[2] ? 2u : 1u};
}

// void SendVibrationValue(const VibrationDeviceHandle &, const VibrationValue &)
static void ov_vib_send(Cpu *c) {
    const uint8_t *h = arg_p(c, 0);
    const VibrationValue *v = arg_p(c, 1);
    knobs();
    if (!h || !v || !rumble_on) return;
    plat_mutex_lock(&lock);
    if (h[2]) vib_high = v->amp_high > v->amp_low ? v->amp_high : v->amp_low;
    else vib_low = v->amp_low > v->amp_high ? v->amp_low : v->amp_high;
    float lo = vib_low, hi = vib_high;
    plat_mutex_unlock(&lock);
    lo *= (float)rumble_gain, hi *= (float)rumble_gain;
    pad_rumble((uint16_t)(fminf(fmaxf(lo, 0), 1) * 65535), (uint16_t)(fminf(fmaxf(hi, 0), 1) * 65535));
}

HOST_OVERRIDE("_ZN2nn3hid14InitializeNpadEv", ov_initialize_npad);
HOST_OVERRIDE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", ov_supported_ids);
HOST_OVERRIDE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", ov_set_supported_styles);
HOST_OVERRIDE("_ZN2nn3hid24GetSupportedNpadStyleSetEv", ov_get_supported_styles);
HOST_OVERRIDE("_ZN2nn3hid15GetNpadStyleSetERKj", ov_style_set);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", ov_state_full_key);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", ov_state_other);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", ov_state_other);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", ov_state_other);
HOST_OVERRIDE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", ov_state_other);
HOST_OVERRIDE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", ov_hold_type);
HOST_OVERRIDE("_ZN2nn3hid21InitializeTouchScreenEv", ov_initialize_touch);
HOST_OVERRIDE("_ZN2nn3hid20GetTouchScreenStatesILm1EEEiPNS0_16TouchScreenStateIXT_EEEi", ov_touch_states);
HOST_OVERRIDE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE",
              ov_controller_support);
HOST_OVERRIDE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_"
              "12NpadStyleTagEEE", ov_vib_handles);
HOST_OVERRIDE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", ov_vib_init);
HOST_OVERRIDE("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", ov_vib_info);
HOST_OVERRIDE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", ov_vib_send);
