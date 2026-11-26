// GPU breadcrumbs for a lost device (vk.h vk_crumb). VK_AMD_buffer_marker writes a 32-bit code into a buffer from
// the command stream: at TOP_OF_PIPE once the command processor reaches the marker, at BOTTOM_OF_PIPE once every
// command before it has finished. Each crumb writes both, into two words of one host-coherent buffer, so after a
// device loss word 0 names the newest point the GPU reached and word 1 the newest point it had finished: the hang lies
// between them. A code is the crumb kind in the top 4 bits and its number in the low 28. The markers cost two small
// writes per crumb, and crumbs mark submissions, upload batches, ASTC chunks and presents, not draws.
//
// Pass marks (vk.h vk_pass_mark_begin) use words 2 and 3 of the same buffer: the replay marks the start and the end of
// every render pass and dispatch, and a ring of records says what each mark was. The design follows Savage Wind
// Recomp's GPU checkpoints (refs/SavageWindRecomp runtime/nvn/vulkan/exec.c ck_mark and vk_checkpoints_report: a mark
// per pass and dispatch, numbers in a ring of 4096, text only at the device loss); the code is ours.
//
// From TotK Recomp runtime/vk_crumbs.c @ ae30a61 (MIT). fastball: the port had left these as stubs (gfx_stubs.c), and
// an AMD Radeon 8060S (Ryzen AI Max+ 395, driver 26.10.32.01) lost the device on Franchise's lineup screen (#32) with
// nothing in the log to say where: that driver fails vkGetDeviceFaultInfoEXT after the loss. The ring of mark records
// and vk_pass_mark_begin / vk_pass_mark_end / vk_pass_mark_record are in vk_passtime.c, which also times the marks
// (MLB_TRACE=perf); this file writes the marks into the command stream and reports them. Off on other GPUs unless
// MLB_PASS_CRUMBS=1 (vk_crumbs_init): then every call returns at once.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "vk.h"

static PFN_vkCmdWriteBufferMarkerAMD write_marker;
static VkBuffer buf;
static VmaAllocation alloc;
static volatile uint32_t *words;   // 0, 1: crumbs; 2, 3: pass marks

// Pass marks (below)
int vk_pass_marks_on;
static int pm_amd;                          // marks into words 2 and 3
static PFN_vkCmdSetCheckpointNV set_checkpoint;
static void pass_marks_init(VkCtx *c);

static void buffer_init(VkCtx *c) {
    write_marker = (PFN_vkCmdWriteBufferMarkerAMD)vkGetDeviceProcAddr(c->device, "vkCmdWriteBufferMarkerAMD");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    VmaAllocationInfo info;
    if (!write_marker || vmaCreateBuffer(c->vma, &bi, &ai, &buf, &alloc, &info) != VK_SUCCESS) {
        rt_log("vk: GPU breadcrumbs unavailable");
        write_marker = NULL;
        return;
    }
    words = info.pMappedData;
    words[0] = words[1] = words[2] = words[3] = 0;
    rt_log("vk: GPU breadcrumbs on (VK_AMD_buffer_marker)");
}

// fastball: unset, on for AMD GPUs only. NVIDIA's driver offers VK_AMD_buffer_marker too, and the marks' cost there was
// not measured; the dev machine's frame rate is the one the user watches. MLB_PASS_CRUMBS=1 turns them on anywhere.
void vk_crumbs_init(VkCtx *c) {
    const char *e = cfg_get("TOTK_PASS_CRUMBS");
    int want = e && *e ? strcmp(e, "0") != 0 : c->props.vendorID == 0x1002;
    if (c->has_buffer_marker && want) buffer_init(c);
    pass_marks_init(c);
}

void vk_crumbs_release(void) {
    VkCtx *c = vk_ctx();
    if (buf && c) vmaDestroyBuffer(c->vma, buf, alloc);
    buf = VK_NULL_HANDLE, alloc = NULL, words = NULL, write_marker = NULL;
    vk_pass_marks_on = pm_amd = 0, set_checkpoint = NULL;
}

void vk_crumb(VkCommandBuffer cb, int kind, uint64_t n) {
    if (!write_marker || !cb) return;
    uint32_t code = (uint32_t)kind << 28 | (uint32_t)(n & 0x0fffffff);
    write_marker(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, buf, 0, code);
    write_marker(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, buf, 4, code);
}

static void describe(uint32_t code, char *o, size_t size) {
    static const char *const kinds[] = {"nothing", "the start of a part of submission", "the end of submission",
                                        "the start of upload batch", "the end of upload batch",
                                        "ASTC decode chunk", "the start of present",
                                        "the end of present"};
    unsigned k = code >> 28;
    if (!code) snprintf(o, size, "nothing yet");
    else if (k < sizeof kinds / sizeof *kinds) snprintf(o, size, "%s %u", kinds[k], code & 0x0fffffff);
    else snprintf(o, size, "unknown code %#x", code);
}

void vk_crumbs_report(void) {
    if (!words) return;
    char reached[96], finished[96];
    describe(words[0], reached, sizeof reached);
    describe(words[1], finished, sizeof finished);
    rt_log("vk: GPU breadcrumbs: the GPU last reached %s; everything before %s had finished", reached, finished);
}

// ---- pass marks
//
// Cost per pass or dispatch run: two commands, one at the start and one at the end, each a marker write (no wait: the
// command processor writes TOP_OF_PIPE at once and BOTTOM_OF_PIPE as an end-of-pipe event), and about twenty stores
// into vk_passtime.c's ring. Per draw nothing, except that the branch of vk_exec_draw.c prepare_draw that records
// changed pipeline state also notes the shaders of a pass's first draw.

// TOTK_PASS_CRUMBS: unset, buffer marks on AMD GPUs (vk_crumbs_init); 1, buffer marks wherever VK_AMD_buffer_marker
// exists and NVIDIA checkpoints; 0, neither.
int vk_pass_marks_want_nv(void) {
    const char *e = cfg_get("TOTK_PASS_CRUMBS");
    return e && *e && strcmp(e, "0") != 0;
}

static void pass_marks_init(VkCtx *c) {
    const char *e = cfg_get("TOTK_PASS_CRUMBS");
    int off = e && !strcmp(e, "0");
    set_checkpoint = !off && c->has_nv_checkpoints
                         ? (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(c->device, "vkCmdSetCheckpointNV") : NULL;
    pm_amd = !off && write_marker && words;
    vk_pass_marks_on = pm_amd || set_checkpoint;
    if (vk_pass_marks_on)
        rt_log("vk: pass marks on (%s%s%s; MLB_PASS_CRUMBS=0 turns them off)", pm_amd ? "VK_AMD_buffer_marker" : "",
               pm_amd && set_checkpoint ? " and " : "", set_checkpoint ? "VK_NV_device_diagnostic_checkpoints" : "");
    else if (off)
        rt_log("vk: pass marks off (MLB_PASS_CRUMBS=0)");
}

void vk_pass_mark_cmd(VkCommandBuffer cb, uint32_t id, int end) {
    if (pm_amd)
        write_marker(cb, end ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, buf,
                     end ? 12 : 8, id);
    if (set_checkpoint) set_checkpoint(cb, (const void *)(uintptr_t)((uint64_t)id << 1 | (end != 0)));
}

int vk_pass_marks_read(uint32_t *started, uint32_t *finished) {
    if (!pm_amd) return 0;
    *started = words[2], *finished = words[3];
    return 1;
}

// The first 8 bytes of the shader's input id (gfx_shader.c), the start of its stored input's name <cache>/in/<id>.in.
static unsigned long long shader_id(const GfxShader *s) {
    const uint8_t *iid = s ? gfx_shader_input_id(s) : NULL;
    if (!iid) return 0;
    unsigned long long v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | iid[i];
    return v;
}

static void describe_mark(uint32_t id, char *o, size_t size) {
    const VkPassMark *m = vk_pass_mark_record(id);
    if (!id) {
        snprintf(o, size, "nothing yet");
        return;
    }
    if (!m) {
        snprintf(o, size, "no record (a later mark took its slot)");
        return;
    }
    if (m->kind == VK_MARK_OTHER) {   // fastball: the port's own work (vk_pass_mark_other)
        snprintf(o, size, "port work of submission %llu: %s", (unsigned long long)m->fence, m->label ? m->label : "?");
        return;
    }
    if (m->kind == VK_MARK_DISPATCH) {
        // fastball: a mark covers a run of chained dispatches (vk_exec_draw.c dispatch_run_mark): w is the run's
        // workgroups, draws its dispatches, last[0] its last shader
        snprintf(o, size, "dispatch run %u of submission %llu: %u dispatches, %u workgroups%s, cs %016llx..%016llx",
                 m->step, (unsigned long long)m->fence, m->draws, m->w, m->w ? "" : " (indirect)",
                 shader_id(m->first[0]), shader_id(m->last[0] ? m->last[0] : m->first[0]));
        return;
    }
    size_t n = (size_t)snprintf(o, size, "pass %u of submission %llu: %ux%u, %u layer%s, colour formats", m->step,
                                (unsigned long long)m->fence, m->w, m->h, m->d, m->d == 1 ? "" : "s");
    for (int i = 0; i < m->ncolor && i < 8 && n < size; i++)
        n += (size_t)snprintf(o + n, size - n, "%s%d", i ? "," : " ", (int)m->color[i]);
    if (!m->ncolor && n < size) n += (size_t)snprintf(o + n, size - n, " none");
    if (n < size) n += (size_t)snprintf(o + n, size - n, ", depth format %d", (int)m->depth);
    if (n >= size) return;
    if (m->draws == UINT32_MAX)
        snprintf(o + n, size - n, ", still recording; first draw vs %016llx fs %016llx", shader_id(m->first[0]),
                 shader_id(m->first[1]));
    else if (!m->draws)
        snprintf(o + n, size - n, ", no draws");
    else
        snprintf(o + n, size - n, ", %u draws; first vs %016llx fs %016llx, last vs %016llx fs %016llx", m->draws,
                 shader_id(m->first[0]), shader_id(m->first[1]), shader_id(m->last[0]), shader_id(m->last[1]));
}

void vk_pass_marks_format(const char *source, uint32_t started, uint32_t finished, char *o, size_t size) {
    char a[640], b[640];
    describe_mark(started, a, sizeof a);
    describe_mark(finished, b, sizeof b);
    snprintf(o, size,
             "vk: pass marks (%s): the GPU last started mark %u and last finished mark %u\n"
             "vk:   last started %u: %s\n"
             "vk:   last finished %u: %s",
             source, (unsigned)started, (unsigned)finished, (unsigned)started, a, (unsigned)finished, b);
}

static void log_lines(char *t) {
    for (char *line = t, *nl; line && *line; line = nl ? nl + 1 : NULL) {
        nl = strchr(line, '\n');
        if (nl) *nl = 0;
        rt_log("%s", line);
    }
}

// vkGetQueueCheckpointData2NV, else vkGetQueueCheckpointDataNV, on the graphics queue (the replay's): one entry per
// stage with the last checkpoint that stage executed. A checkpoint is the mark's id times 2, plus 1 at its end, so the
// top-of-pipe entry names the last start or end the GPU reached, and the bottom-of-pipe entry the last one before which
// everything had finished: at a start, that is the end of the mark before it.
static void nv_report(void) {
    VkCtx *c = vk_ctx();
    if (!c || !set_checkpoint) return;
    PFN_vkGetQueueCheckpointData2NV get2 =
        (PFN_vkGetQueueCheckpointData2NV)vkGetDeviceProcAddr(c->device, "vkGetQueueCheckpointData2NV");
    PFN_vkGetQueueCheckpointDataNV get1 =
        (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(c->device, "vkGetQueueCheckpointDataNV");
    enum { MAXN = 32 };
    uint64_t stage[MAXN], marker[MAXN];
    uint32_t n = 0;
    if (get2) {
        VkCheckpointData2NV d[MAXN];
        get2(c->gfx_queue, &n, NULL);
        if (n > MAXN) n = MAXN;
        for (uint32_t i = 0; i < n; i++) d[i] = (VkCheckpointData2NV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV};
        get2(c->gfx_queue, &n, d);
        for (uint32_t i = 0; i < n; i++) stage[i] = d[i].stage, marker[i] = (uint64_t)(uintptr_t)d[i].pCheckpointMarker;
    } else if (get1) {
        VkCheckpointDataNV d[MAXN];
        get1(c->gfx_queue, &n, NULL);
        if (n > MAXN) n = MAXN;
        for (uint32_t i = 0; i < n; i++) d[i] = (VkCheckpointDataNV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV};
        get1(c->gfx_queue, &n, d);
        for (uint32_t i = 0; i < n; i++) stage[i] = d[i].stage, marker[i] = (uint64_t)(uintptr_t)d[i].pCheckpointMarker;
    } else {
        rt_log("vk: pass marks: no vkGetQueueCheckpointDataNV entry point");
        return;
    }
    uint32_t started = 0, finished = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t id = (uint32_t)(marker[i] >> 1);
        int end = (int)(marker[i] & 1);
        rt_log("vk: checkpoint of stage %#llx: the %s of mark %u", (unsigned long long)stage[i], end ? "end" : "start", id);
        if (stage[i] == VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT) started = id;
        if (stage[i] == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT) finished = end ? id : id ? id - 1 : 0;
    }
    if (!n) rt_log("vk: pass marks: the driver reported no checkpoints");
    char t[2048];
    vk_pass_marks_format("VK_NV_device_diagnostic_checkpoints", started, finished, t, sizeof t);
    log_lines(t);
}

void vk_pass_marks_report(void) {
    if (!vk_pass_marks_on) return;
    uint32_t started, finished;
    char t[2048];
    if (vk_pass_marks_read(&started, &finished)) {
        vk_pass_marks_format("VK_AMD_buffer_marker", started, finished, t, sizeof t);
        log_lines(t);
    }
    nv_report();
}
