// time:u: clocks and time zones. Command ids per Switchbrew ("Glue services": IStaticService, ISystemClock,
// ISteadyClock, ITimeZoneService); the shared memory layout as SDK 19.3.11 reads it (nn::time::detail::
// SharedMemoryClient, disassembled; docs/runtime-port.md). Our own implementation.
//
// nntimeStartup (inside nn::init::Start) opens time:u and maps the service's 0x1000-byte shared memory; from then on
// the SDK reads the clocks from that page without IPC:
//   0x000  steady clock context        counter, 2 x {u64 internal offset ns, UUID source}                (0x18 each)
//   0x038  local (user) clock context  counter, 2 x SystemClockContext {s64 offset s, s64 steady s, UUID} (0x20)
//   0x080  network clock context       same
//   0x0C8  automatic correction        counter, 2 x bool
//   0x0D0  steady clock adjustment     counter, 2 x {s64 base ns, s64 scale, s64 shift, s64 offset ns, s64 limit ns,
//                                      UUID source} (0x38 each); the steady time point is
//                                      (now_ns + clamp(((now_ns - base) * scale >> shift) + offset, limit)) / 1e9
// Each block is a lock-free pair: readers take element counter & 1 and retry when the counter moved.
//
// The steady clock is the guest tick (it starts near 0 at boot, with a new source UUID every run, as after a reboot).
// The system clocks are the host's UTC clock: offset = host now - steady now, taken once at startup. The time zone is
// the host's (MLB_TIMEZONE=local, the default) or UTC (MLB_TIMEZONE=utc).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cfg.h"
#include "cpu.h"
#include "host.h"
#include "ipc.h"
#include "platform/platform.h"

// The C library's 64-bit time conversions: the MSVC CRT's _*64 functions, POSIX's *_r and timegm elsewhere.
#ifdef _WIN32
typedef __time64_t Time64;
#define local_tm(t, tm) (_localtime64_s((tm), (t)) == 0)
#define utc_tm(t, tm) (_gmtime64_s((tm), (t)) == 0)
#define tm_utc _mkgmtime64
#define tm_local _mktime64
#else
typedef time_t Time64;
#define local_tm(t, tm) (localtime_r((t), (tm)) != NULL)
#define utc_tm(t, tm) (gmtime_r((t), (tm)) != NULL)
#define tm_utc timegm
#define tm_local mktime
#endif

#define TICK_HZ 19200000ull
#define SHM_SIZE 0x1000ull

typedef struct { uint8_t b[16]; } Uuid;
typedef struct { int64_t offset, steady; Uuid source; } SystemClockContext;   // 0x20
typedef struct { int64_t time_point; Uuid source; } SteadyClockTimePoint;     // 0x18
_Static_assert(sizeof(SystemClockContext) == 0x20 && sizeof(SteadyClockTimePoint) == 0x18, "time layouts");

static PlatMutex time_lock = PLAT_MUTEX_INIT;
static RtSharedMem *shm;
static Uuid source;
static SystemClockContext clock_ctx;   // user, local and network clocks alike: offline there is one time

static int64_t steady_ns(void) { return (int64_t)((unsigned __int128)rt_ticks() * 1000000000ull / TICK_HZ); }
static int64_t steady_s(void) { return steady_ns() / 1000000000; }

static void shm_put(uint64_t off, const void *v, uint64_t n) { mem_shmem_write(shm, off, v, n); }

static void time_init(void) {   // holding time_lock
    if (shm) return;
    shm = mem_shmem_new("time", SHM_SIZE);
    if (!plat_random(&source, sizeof source)) rt_fatal("time: no random bytes for the clock source id");
    int64_t now_s = steady_s();
    clock_ctx = (SystemClockContext){(int64_t)time(NULL) - now_s, now_s, source};
    // Every block at counter 0, element 0.
    struct { uint64_t offset; Uuid source; } steady = {0, source};
    shm_put(0x008, &steady, sizeof steady);
    shm_put(0x040, &clock_ctx, sizeof clock_ctx);
    shm_put(0x088, &clock_ctx, sizeof clock_ctx);
    uint8_t automatic = 0;
    shm_put(0x0CC, &automatic, 1);
    struct { int64_t base, scale, shift, offset, limit; Uuid source; } adjust = {0, 0, 0, 0, 0, source};
    _Static_assert(sizeof adjust == 0x38, "steady clock adjustment layout");
    shm_put(0x0D8, &adjust, sizeof adjust);
    rt_log("time: clocks from the host (UTC %lld), steady clock from the guest tick, time zone %s",
           (long long)time(NULL), cfg_value("MLB_TIMEZONE"));
}

static SteadyClockTimePoint steady_now(void) { return (SteadyClockTimePoint){steady_s(), source}; }

static uint32_t event_copy(uint32_t *w) {
    if (!*w) *w = kern_event_new();
    return kern_event_readable(*w);
}

static void system_clock(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    static uint32_t operation;
    switch (rq->cmd) {
    case 0: {   // GetCurrentTime -> s64 posix seconds
        int64_t t = clock_ctx.offset + steady_s();
        IPC_OUT(rs, t);
        return;
    }
    case 2: IPC_OUT(rs, clock_ctx); return;   // GetSystemClockContext
    case 4: rs->copy[rs->ncopy++] = event_copy(&operation); return;   // GetOperationEventReadableHandle
    default: ipc_unimplemented(s, rq);
    }
}

static void steady_clock(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: {   // GetCurrentTimePoint
        SteadyClockTimePoint p = steady_now();
        IPC_OUT(rs, p);
        return;
    }
    case 2: case 200: {   // GetTestOffset, GetInternalOffset -> s64 (ns)
        int64_t v = 0;
        IPC_OUT(rs, v);
        return;
    }
    case 100: {   // GetRtcValue -> s64 seconds
        int64_t v = (int64_t)time(NULL);
        IPC_OUT(rs, v);
        return;
    }
    case 101: {   // IsRtcResetDetected -> bool
        uint8_t v = 0;
        IPC_OUT(rs, v);
        return;
    }
    case 102: {   // GetSetupResultValue -> result
        uint32_t v = 0;
        IPC_OUT(rs, v);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

// ---- time zone: the host's rules through the C library
typedef struct { int16_t year; int8_t month, day, hour, minute, second, pad; } CalendarTime;   // month 1-12
typedef struct { uint32_t day_of_week, day_of_year; char tz_name[8]; uint32_t is_dst; int32_t utc_offset; } CalendarInfo;
_Static_assert(sizeof(CalendarTime) == 8 && sizeof(CalendarInfo) == 0x18, "calendar layouts");

static int tz_local(void) {
    const char *v = cfg_value("MLB_TIMEZONE");
    if (!strcmp(v, "local")) return 1;
    if (!strcmp(v, "utc")) return 0;
    rt_fatal("time: MLB_TIMEZONE=%s: use local or utc", v);
}

static const char *location_name(void) { return tz_local() ? "localtime" : "UTC"; }

static void to_calendar(int64_t t, CalendarTime *ct, CalendarInfo *ci) {
    Time64 tt = t;
    struct tm tm;
    int local = tz_local();
    if (!(local ? local_tm(&tt, &tm) : utc_tm(&tt, &tm))) rt_fatal("time: cannot convert %lld", (long long)t);
    struct tm copy = tm;
    int32_t offset = local ? (int32_t)(tm_utc(&copy) - tt) : 0;
    *ct = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour,
                         (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    *ci = (CalendarInfo){(uint32_t)tm.tm_wday, (uint32_t)tm.tm_yday, {0}, tm.tm_isdst > 0, offset};
    // Zone abbreviations as tzdata writes zones without one: "UTC", or the offset ("-05", "+0530").
    int32_t m = offset / 60, am = m < 0 ? -m : m;
    if (!offset) memcpy(ci->tz_name, "UTC", 3);
    else if (am % 60) snprintf(ci->tz_name, sizeof ci->tz_name, "%c%02d%02d", m < 0 ? '-' : '+', am / 60, am % 60);
    else snprintf(ci->tz_name, sizeof ci->tz_name, "%c%02d", m < 0 ? '-' : '+', am / 60);
}

static int64_t from_calendar(const CalendarTime *ct) {
    struct tm tm = {.tm_year = ct->year - 1900, .tm_mon = ct->month - 1, .tm_mday = ct->day, .tm_hour = ct->hour,
                    .tm_min = ct->minute, .tm_sec = ct->second, .tm_isdst = -1};
    Time64 t = tz_local() ? tm_local(&tm) : tm_utc(&tm);
    if (t == -1) rt_fatal("time: calendar %d-%d-%d %d:%d:%d has no posix time", ct->year, ct->month, ct->day, ct->hour,
                          ct->minute, ct->second);
    return t;
}

static void time_zone(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: {   // GetDeviceLocationName -> LocationName (0x24 bytes)
        char name[0x24] = {0};
        memcpy(name, location_name(), strlen(location_name()));
        IPC_OUT(rs, name);
        return;
    }
    case 2: {   // GetTotalLocationNameCount -> u32
        uint32_t n = 1;
        IPC_OUT(rs, n);
        return;
    }
    case 4: {     // LoadTimeZoneRule(LocationName) -> buffer 0x16: TimeZoneRule (0x4000 bytes)
        // fastball: the game loads its device location's rule when it goes online (the mlb_login.post_update
        // request, main+0xaa201c). The rule stays opaque to the game: it only hands it back to ToCalendarTime /
        // ToPosixTime (100 / 201), which use the host's rules like the *WithMyRule commands. The location name is kept
        // at the start of the buffer for the log.
        const IpcBuffer *b = rq->nb && rq->b[0].size ? &rq->b[0] : NULL;
        const char *name = ipc_arg(rq, 0, 0x24);
        if (b) {
            uint8_t *out = ipc_guest(b->addr, b->size);
            memset(out, 0, b->size);
            memcpy(out, name, b->size < 0x24 ? b->size : 0x24);
        }
        rt_log("time: LoadTimeZoneRule(%.36s): the host's rules (MLB_TIMEZONE=%s)", name, tz_local() ? "local" : "utc");
        return;
    }
    case 100:     // ToCalendarTime(s64 posix, buffer 0x15: TimeZoneRule) -> CalendarTime, CalendarAdditionalInfo
    case 101: {   // ToCalendarTimeWithMyRule(s64 posix) -> CalendarTime, CalendarAdditionalInfo
        struct { CalendarTime t; CalendarInfo i; } out;
        to_calendar(IPC_ARG(rq, int64_t, 0), &out.t, &out.i);
        IPC_OUT(rs, out);
        return;
    }
    case 201: {   // ToPosixTime(CalendarTime, buffer 0x15: TimeZoneRule) -> u32 count, buffer 0xA: s64[]
        CalendarTime ct;
        memcpy(&ct, ipc_arg(rq, 0, sizeof ct), sizeof ct);
        int64_t t = from_calendar(&ct);
        if (!rq->nc || rq->c[0].size < sizeof t) rt_fatal("time: ToPosixTime without an output buffer");
        memcpy(ipc_guest(rq->c[0].addr, sizeof t), &t, sizeof t);
        rs->xout[rs->nxout++] = (IpcBuffer){rq->c[0].addr, sizeof t, 0};
        uint32_t n = 1;
        IPC_OUT(rs, n);
        return;
    }
    case 202: {   // ToPosixTimeWithMyRule(CalendarTime) -> u32 count, buffer 0xA: s64[]
        CalendarTime ct;
        memcpy(&ct, ipc_arg(rq, 0, sizeof ct), sizeof ct);
        int64_t t = from_calendar(&ct);
        if (!rq->nc || rq->c[0].size < sizeof t) rt_fatal("time: ToPosixTimeWithMyRule without an output buffer");
        memcpy(ipc_guest(rq->c[0].addr, sizeof t), &t, sizeof t);
        rs->xout[rs->nxout++] = (IpcBuffer){rq->c[0].addr, sizeof t, 0};
        uint32_t n = 1;
        IPC_OUT(rs, n);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

// ---- IStaticService
void time_u_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    plat_mutex_lock(&time_lock);
    time_init();
    plat_mutex_unlock(&time_lock);
    switch (rq->cmd) {
    case 0: case 1: case 4: case 5:   // GetStandard{User,Network,Local}SystemClock, GetEphemeralNetworkSystemClock
        rs->move[rs->nmove++] = ipc_session_new("time:ISystemClock", system_clock, NULL, NULL);
        return;
    case 2: rs->move[rs->nmove++] = ipc_session_new("time:ISteadyClock", steady_clock, NULL, NULL); return;
    case 3: rs->move[rs->nmove++] = ipc_session_new("time:ITimeZoneService", time_zone, NULL, NULL); return;
    case 20: rs->copy[rs->ncopy++] = kern_shmem_handle(shm); return;   // GetSharedMemoryNativeHandle
    case 100: case 200: {   // IsStandardUserSystemClockAutomaticCorrectionEnabled,
                            // IsStandardNetworkSystemClockAccuracySufficient -> bool
        uint8_t v = 0;
        IPC_OUT(rs, v);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}
