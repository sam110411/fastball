// fastball, our own: GPU time per render pass and per run of compute dispatches (MLB_TRACE=perf). The pass marks
// of vk.h (vk_exec.c pass begin and end; vk_exec_draw.c one per run of dispatches with no barrier between them) each
// get a pair of timestamps written at ALL_COMMANDS, so a mark measures its own work after the work before it finished.
// Marks sit next to barriers, which drain the GPU anyway: a mark per dispatch drained it 1,500-3,600 more times a
// frame (+4-8 ms in the draft cutscene). Results are read without waiting, in mark order, once the mark's submission
// completed; every 300 presents a line lists the passes and dispatch runs that took the most GPU time per frame, and
// the time between marks of one submission (barriers, copies, uploads). A mark whose ring slot is still in flight
// gets no timestamps.
//
// The same marks are TotK's breadcrumbs for device-lost reports (vk_crumbs.c, on where VK_AMD_buffer_marker exists):
// a mark is made when either wants it, the ring below is the record both read, and only a timed mark (MLB_TRACE=perf,
// its slot's queries free) gets timestamps.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_rec.h"

static VkPassMark ring[VK_PASS_MARKS];
static uint32_t ended[VK_PASS_MARKS];   // the id whose end was recorded in that slot
static uint8_t timed[VK_PASS_MARKS];    // the mark in that slot has its start timestamp
static VkQueryPool qp;
static int state;                       // 0 not tried, 1 on, -1 off
static double tick_ns;
static uint32_t next_id = 1, collected = 1;

#define AGG_N 1024
static struct { char key[96]; double ns; uint64_t n, disp, groups, maxg; } agg[AGG_N];
static int nagg;
static uint64_t frame0, n_marks, n_skipped, prev_end, prev_fence;
static int prev_kind;
static double total_ns, gap_ns, gap_max_ns;

static int init(void) {
    if (state) return state > 0;
    state = -1;
    VkCtx *c = vk_ctx();
    if (!c->props.limits.timestampComputeAndGraphics) return 0;
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                .queryCount = 2 * VK_PASS_MARKS};
    if (vkCreateQueryPool(c->device, &qi, NULL, &qp) != VK_SUCCESS) return 0;
    vkResetQueryPool(c->device, qp, 0, 2 * VK_PASS_MARKS);
    tick_ns = c->props.limits.timestampPeriod;
    state = 1;
    rt_log("vk: GPU time per pass and dispatch: on (MLB_TRACE=perf)");
    return 1;
}

static void id_hex(const GfxShader *s, char *out) {
    const uint8_t *id = s ? gfx_shader_input_id(s) : NULL;
    if (id) snprintf(out, 9, "%02x%02x%02x%02x", id[0], id[1], id[2], id[3]);
    else snprintf(out, 9, "-");
}

static void key_of(const VkPassMark *m, char *key, size_t n) {
    char a[9], b[9];
    if (m->kind == VK_MARK_OTHER) {
        snprintf(key, n, "port: %s", m->label ? m->label : "?");
    } else if (m->kind == VK_MARK_DISPATCH) {
        id_hex(m->first[0], a), id_hex(m->last[0], b);
        if (m->last[0] && m->last[0] != m->first[0]) snprintf(key, n, "dispatch run cs %s..%s", a, b);
        else snprintf(key, n, "dispatch run cs %s", a);
    } else {
        id_hex(m->first[1], a), id_hex(m->last[1], b);
        snprintf(key, n, "pass %ux%u x%u c%d/%d d%d fs %s..%s", m->w, m->h, m->d, m->ncolor ? (int)m->color[0] : 0,
                 m->ncolor, (int)m->depth, a, b);
    }
}

static void add(const char *key, double ns, const VkPassMark *m) {
    unsigned h = 2166136261u;
    for (const char *p = key; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    for (unsigned i = h % AGG_N, k = 0; k < AGG_N; k++, i = (i + 1) % AGG_N) {
        if (!agg[i].key[0]) {
            if (nagg >= AGG_N * 3 / 4) return;
            snprintf(agg[i].key, sizeof agg[i].key, "%s", key);
            nagg++;
        }
        if (!strcmp(agg[i].key, key)) {
            agg[i].ns += ns, agg[i].n++;
            if (m->kind == VK_MARK_DISPATCH) {
                agg[i].disp += m->draws, agg[i].groups += m->w;
                if (m->h > agg[i].maxg) agg[i].maxg = m->h;
            }
            return;
        }
    }
}

static int cmp_ns(const void *x, const void *y) {
    double a = agg[*(const int *)x].ns, b = agg[*(const int *)y].ns;
    return a < b ? 1 : a > b ? -1 : 0;
}

// Reads the finished marks, in order, and every 600 presents logs the top ones per frame.
static void collect(void) {
    VkCtx *c = vk_ctx();
    uint64_t done = vk_sync_fence_done();
    for (; collected != next_id; collected++) {
        uint32_t s = collected % VK_PASS_MARKS;
        VkPassMark *m = &ring[s];
        if (m->id != collected || ended[s] != collected) continue;   // overwritten, or never ended
        if (m->fence > done) break;
        uint64_t r[4];
        if (vkGetQueryPoolResults(c->device, qp, 2 * s, 2, sizeof r, r, 2 * sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) < 0 || !r[1] || !r[3])
            continue;
        double ns = r[2] > r[0] ? (double)(r[2] - r[0]) * tick_ns : 0;
        // GPU time between this mark and the one before in the same command stream: not across a port mark, which may
        // sit in a prologue command buffer submitted on its own
        if (m->fence == prev_fence && r[0] > prev_end && m->kind != VK_MARK_OTHER && prev_kind != VK_MARK_OTHER) {
            double g = (double)(r[0] - prev_end) * tick_ns;
            gap_ns += g;
            if (g > gap_max_ns) gap_max_ns = g;
        }
        prev_end = r[2], prev_fence = m->fence, prev_kind = m->kind;
        char key[96];
        key_of(m, key, sizeof key);
        add(key, ns, m);
        total_ns += ns, n_marks++;
    }
    uint64_t fr = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    if (!frame0) frame0 = fr;
    if (fr < frame0 + 300) return;
    double nf = (double)(fr - frame0);
    int idx[AGG_N], n = 0;
    for (int i = 0; i < AGG_N; i++)
        if (agg[i].key[0]) idx[n++] = i;
    qsort(idx, (size_t)n, sizeof *idx, cmp_ns);
    rt_log("perf: frame %llu: GPU time in marked passes and dispatch runs %.2f ms per frame (%.0f marks per frame, %llu "
           "untimed); between marks of a submission %.2f ms per frame (longest %.3f ms); the most expensive:",
           (unsigned long long)fr, total_ns / 1e6 / nf, n_marks / nf, (unsigned long long)n_skipped, gap_ns / 1e6 / nf,
           gap_max_ns / 1e6);
    for (int k = 0; k < n && k < 20; k++) {
        int i = idx[k];
        char extra[128] = "";
        if (agg[i].disp)
            snprintf(extra, sizeof extra, " (%.1f dispatches per frame, %.1f per run, %.1f workgroups each, largest %llu)",
                     agg[i].disp / nf, (double)agg[i].disp / (double)agg[i].n, (double)agg[i].groups / (double)agg[i].disp,
                     (unsigned long long)agg[i].maxg);
        rt_log("perf:   %6.3f ms per frame, %5.1f per frame, %.3f ms each: %s%s", agg[i].ns / 1e6 / nf, agg[i].n / nf,
               agg[i].ns / 1e6 / (double)agg[i].n, agg[i].key, extra);
    }
    memset(agg, 0, sizeof agg);
    nagg = 0, total_ns = gap_ns = gap_max_ns = 0, n_marks = n_skipped = 0, frame0 = fr;
}

VkPassMark *vk_pass_mark_begin(VkCommandBuffer cb, int kind, uint64_t fence) {
    int timing = (rt_trace & RT_TRACE_PERF) && init();
    if (!cb || (!timing && !vk_pass_marks_on)) return NULL;
    if (timing && (next_id & 63) == 0) collect();
    uint32_t id = next_id, s = id % VK_PASS_MARKS;
    VkPassMark *m = &ring[s];
    // its previous mark is still on the GPU: no timing for this one (that mark's queries are in flight)
    int t = timing && !(m->id && m->fence > vk_sync_fence_done());
    if (timing && !t) {
        n_skipped++;
        if (!vk_pass_marks_on) return NULL;
    }
    if (t) vkResetQueryPool(vk_ctx()->device, qp, 2 * s, 2);
    memset(m, 0, sizeof *m);
    m->id = id, m->kind = (uint8_t)kind, m->fence = fence, m->draws = UINT32_MAX;
    ended[s] = 0, timed[s] = (uint8_t)t;
    if (vk_pass_marks_on) rec_pass_mark(cb, id, 0);
    if (t) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, 2 * s);
    next_id = id + 1 ? id + 1 : 1;
    return m;
}

// vk_crumbs.c: the record of mark id at a device loss; NULL once a later mark took its slot
const VkPassMark *vk_pass_mark_record(uint32_t id) {
    const VkPassMark *m = &ring[id % VK_PASS_MARKS];
    return id && m->id == id ? m : NULL;
}

VkPassMark *vk_pass_mark_other(VkCommandBuffer cb, uint64_t fence, const char *label) {
    VkPassMark *m = vk_pass_mark_begin(cb, VK_MARK_OTHER, fence);
    if (m) m->label = label, m->draws = 0;
    return m;
}

void vk_pass_mark_end(VkCommandBuffer cb, const VkPassMark *m) {
    if (!m || !cb) return;
    uint32_t s = m->id % VK_PASS_MARKS;
    if (vk_pass_marks_on) rec_pass_mark(cb, m->id, 1);
    if (!timed[s] || ring[s].id != m->id) return;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qp, 2 * s + 1);
    ended[s] = m->id;
}
