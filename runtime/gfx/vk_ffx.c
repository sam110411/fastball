// fastball, our own: AMD FSR 3.1 and FSR 4 on MLB's 3D scene (vk_ffx.h).
//
// The upscalers are AMD's signed Vulkan runtime from the FSR SDK 2.4.0 (revision 7539ae8b; runtime/third_party/ffx has
// the API headers this file compiles against): amd_fidelityfx_loader_vk.dll, the API, and
// amd_fidelityfx_upscaler_vk.dll, the upscalers. Both are loaded at run time from the folder of mlb.exe; the setup
// downloads them (installer/steps.py), tools/fetch_ffx.py does for the dev tree. No AMD code is compiled in.
//
// The runtime lists the upscaler versions the GPU can run (ffxQueryDescGetVersions): on SDK 2.4.0 "3.1.6" (FSR 3.1,
// the analytical upscaler, every Vulkan 1.3 GPU), "2.3.4" (FSR 2, unused here) and, on the Radeons AMD made it for
// and with AMD's own driver, "4.x" (FSR 4, the machine-learning upscaler: RX 9000 with an FP8 model, RX 6000 / 7000
// and some Ryzen graphics with INT8 ones). A context is made for one version with ffxOverrideVersion. This follows
// Super Odyssey PC Recomp's runtime/nvn/vulkan/fsr3_sdk.c (0.4.8), which found how the versions are picked; FSR 4
// also takes the API version descriptor (ffxCreateContextDescUpscaleVersion), as AMD's headers ask.
//
// Device: AMD's own sample framework (Cauldron2 device_vk.cpp at the same revision) turns on VK_KHR_cooperative_matrix,
// VK_EXT_shader_float8 and VK_KHR_compute_shader_derivatives for FSR 4's models ("DXC emits DerivativeGroupLinearKHR
// for SM 6.6 compute shaders using quad ops (e.g. FSR4 INT8 prepass)"); vk_device.c does the same while MLB_FSR is on.
// Everything else the runtime asks of the device (shaderFloat16, subgroup size control, integer dot products) is core
// Vulkan 1.2 / 1.3, which vk_device.c enables wherever the GPU has it.
//
// The inputs (vk_dlaa.c): the scene's light (linear HDR, B10G11R11 float: FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE, with
// FSR's own exposure), its depth (D32_SFLOAT_S8_UINT here; FSR reads the depth plane of a D32S8 image itself, AMD's
// guide; not inverted, near is 0), the camera motion vectors and the jitter. The output, RGBA16F at the output size,
// replaces the scene for the game's composite draw, exactly as DLSS's does.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// AMD ships the runtime for Windows only: on Linux its DLLs are never found and the menu shows FSR 3.1 / 4 as
// unavailable. The API header's __declspec(dllexport) then has no meaning.
#ifndef _WIN32
#define __declspec(x)
#endif

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "vk_ffx.h"
#include "vk_rec.h"

#include "../third_party/ffx/api/include/vk/ffx_api_vk.h"
#include "../third_party/ffx/upscalers/include/ffx_upscale.h"

static struct {
    int tried, loaded, dlss;            // init ran / a provider is there / DLSS took the scene instead
    const char *why;                    // why FSR is not there (the menu's note)
    void *runtime, *loader;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    uint64_t id31, id4;                 // the version ids of FSR 3.1 and FSR 4 (0: not offered)
    char name31[32], name4[32];
    int failed4;                        // an FSR 4 context could not be made: FSR 3.1 from then on
    int failed;                         // a dispatch failed: FSR off for the run (vk_dlaa.c sees the 0)
    uint64_t last_ns;                   // plat_now_ns of the last FSR frame
    int last_ml;                        // that frame's context was FSR 4
} F;

typedef struct {
    ffxContext ctx;
    int ml;
} Ctx;

int vk_ffx_wanted(void) {
    static int wanted = -1;
    if (wanted < 0) wanted = strcmp(cfg_value("FSR"), "off") != 0;
    return wanted;
}

int vk_ffx_available(void) { return F.loaded && !F.failed; }

int vk_ffx_ml_wanted(void) { return F.id4 && !F.failed4 && !strcmp(cfg_value("FSR_VERSION"), "4"); }

void vk_ffx_ran(void) { F.last_ns = plat_now_ns(); }

const char *vk_ffx_missing(void) {
    static int have = -1;
    if (have < 0) {
        char exe[PLAT_PATH_MAX], path[PLAT_PATH_MAX + 64];
        const char *sep = plat_exe_path(exe, sizeof exe) ? plat_last_sep(exe) : NULL;
        have = sep != NULL;
        static const char *const libs[] = {FFX_LOADER_DLL, FFX_UPSCALER_DLL};
        for (int i = 0; i < 2 && have; i++) {
            snprintf(path, sizeof path, "%.*s/%s", (int)(sep - exe), exe, libs[i]);
            have = plat_file_exists(path);
        }
    }
    if (!have) return "AMD's FSR files are missing (run the setup again)";
    if (F.loaded && !F.id31 && !F.id4) return "AMD's FSR does not run on this graphics card";
    return NULL;
}

int vk_ffx_has4(void) { return F.loaded ? F.id4 != 0 && !F.failed4 : -1; }

const char *vk_ffx_status(void) {
    if (!vk_ffx_wanted()) return NULL;
    if (!F.tried) return "starting";
    if (F.dlss) return "DLSS is on and takes the 3D scene";
    if (!F.loaded) return F.why ? F.why : "not available (see mlb.log)";
    if (F.failed) return "stopped after an error (see mlb.log)";
    int want4 = !strcmp(cfg_value("FSR_VERSION"), "4");
    uint64_t now = plat_now_ns();
    if (!F.last_ns || now - F.last_ns > 1000000000ull) {
        if (want4 && !F.id4) return "waiting for a 3D scene; FSR 4 needs a Radeon that runs it: FSR 3.1 here";
        return "waiting for a 3D scene";
    }
    if (F.last_ml) return "active: FSR 4";
    if (want4) return F.failed4 ? "active: FSR 3.1 (FSR 4 failed to start, see mlb.log)"
                                : "active: FSR 3.1 (FSR 4 needs a Radeon that runs it)";
    return "active: FSR 3.1";
}

void vk_ffx_init(void) {
    if (!vk_ffx_wanted() || F.tried) return;
    F.tried = 1;
    if (vk_dlaa_available()) {
        F.dlss = 1;
        rt_log("fsr: MLB_FSR=%s but DLSS is on and takes the 3D scene: FSR is not started", cfg_value("FSR"));
        return;
    }
    char exe[PLAT_PATH_MAX], path[PLAT_PATH_MAX + 64];
    const char *sep = plat_exe_path(exe, sizeof exe) ? plat_last_sep(exe) : NULL;
    if (!sep) {
        F.why = "cannot find the folder of mlb.exe";
        rt_log("fsr: cannot name the folder of mlb.exe: FSR off");
        return;
    }
    // the upscalers first, from this folder, so the loader finds this copy loaded
    snprintf(path, sizeof path, "%.*s/%s", (int)(sep - exe), exe, FFX_UPSCALER_DLL);
    if (!plat_file_exists(path) || !(F.runtime = plat_lib_open(path))) {
        F.why = FFX_UPSCALER_DLL " is missing next to mlb.exe (run the setup again)";
        rt_log("fsr: %s is missing or does not load: FSR off (the setup downloads it; tools/fetch_ffx.py for the "
               "dev tree)", path);
        return;
    }
    snprintf(path, sizeof path, "%.*s/%s", (int)(sep - exe), exe, FFX_LOADER_DLL);
    if (!plat_file_exists(path) || !(F.loader = plat_lib_open(path))) {
        F.why = FFX_LOADER_DLL " is missing next to mlb.exe (run the setup again)";
        rt_log("fsr: %s is missing or does not load: FSR off", path);
        return;
    }
    F.create = (PfnFfxCreateContext)plat_lib_sym(F.loader, "ffxCreateContext");
    F.destroy = (PfnFfxDestroyContext)plat_lib_sym(F.loader, "ffxDestroyContext");
    F.query = (PfnFfxQuery)plat_lib_sym(F.loader, "ffxQuery");
    F.dispatch = (PfnFfxDispatch)plat_lib_sym(F.loader, "ffxDispatch");
    if (!F.create || !F.destroy || !F.query || !F.dispatch) {
        F.why = "AMD's runtime lacks its entry points (see mlb.log)";
        rt_log("fsr: %s lacks the ffx entry points: FSR off", FFX_LOADER_DLL);
        return;
    }
    const VkCtx *c = vk_ctx();
    VkDeviceContext dc = {c->device, c->phys, vkGetDeviceProcAddr};
    uint64_t n = 16, ids[16] = {0};
    const char *names[16] = {0};
    struct ffxQueryDescGetVersions q = {.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS,
                                        .createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, .device = &dc,
                                        .outputCount = &n, .versionIds = ids, .versionNames = names};
    ffxReturnCode_t r = F.query(NULL, &q.header);
    if (r != FFX_API_RETURN_OK) {
        F.why = "AMD's runtime did not answer (see mlb.log)";
        rt_log("fsr: the version query failed (%u): FSR off", r);
        return;
    }
    if (n > 16) n = 16;
    char list[256];
    int at = 0;
    list[0] = 0;
    for (uint64_t i = 0; i < n; i++) {
        const char *nm = names[i] ? names[i] : "?";
        unsigned major = 0, minor = 0;
        sscanf(nm, "%u.%u", &major, &minor);
        if (major == 3 && minor == 1 && !F.id31) F.id31 = ids[i], snprintf(F.name31, sizeof F.name31, "%s", nm);
        if (major == 4 && !F.id4) F.id4 = ids[i], snprintf(F.name4, sizeof F.name4, "%s", nm);
        if (at < (int)sizeof list - 1) at += snprintf(list + at, sizeof list - at, "%s%s", i ? ", " : "", nm);
    }
    rt_log("fsr: AMD's runtime offers %s on %s (FSR 3.1: %s, FSR 4: %s; MLB_FSR=%s, MLB_FSR_VERSION=%s)", list,
           c->props.deviceName, F.id31 ? F.name31 : "no", F.id4 ? F.name4 : "no (needs a Radeon that runs it and "
           "AMD's driver)", cfg_value("FSR"), cfg_value("FSR_VERSION"));
    if (!F.id31 && !F.id4) {
        F.why = "AMD's runtime offers no FSR 3.1 or FSR 4 for this GPU";
        return;
    }
    F.loaded = 1;
}

// MLB_FSR_DEBUG=1 (test): AMD's runtime checks what it is given and says what it finds wrong here
static void ffx_message(uint32_t type, const wchar_t *msg) {
    static int n;
    if (n++ < 50) rt_log("fsr: runtime %s: %ls", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", msg);
}

static Ctx *ctx_make(const VkFfxFrame *f, int ml) {
    const VkCtx *c = vk_ctx();
    static int debug = -1;
    if (debug < 0) debug = rt_knob("FSR_DEBUG", "0");
    struct ffxCreateBackendVKDesc be = {.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK,
                                        .deviceContext = {c->device, c->phys, vkGetDeviceProcAddr}};
    struct ffxOverrideVersion ov = {.header = {FFX_API_DESC_TYPE_OVERRIDE_VERSION, &be.header},
                                    .versionId = ml ? F.id4 : F.id31};
    struct ffxCreateContextDescUpscaleVersion api = {.header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION,
                                                                &ov.header},
                                                     .version = FFX_UPSCALER_VERSION};
    struct ffxCreateContextDescUpscale d = {
        .header = {FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, ml ? &api.header : &ov.header},
        .flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
                 (debug ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING : 0),
        .maxRenderSize = {f->w, f->h}, .maxUpscaleSize = {f->ow, f->oh}, .fpMessage = debug ? ffx_message : NULL};
    ffxContext ctx = NULL;
    ffxReturnCode_t r = F.create(&ctx, &d.header, NULL);
    if (r != FFX_API_RETURN_OK || !ctx) {
        rt_log("fsr: FSR %s context at %ux%u -> %ux%u failed (%u)", ml ? "4" : "3.1", f->w, f->h, f->ow, f->oh, r);
        return NULL;
    }
    Ctx *x = calloc(1, sizeof *x);
    if (!x) return F.destroy(&ctx, NULL), NULL;
    x->ctx = ctx, x->ml = ml;
    struct ffxQueryGetProviderVersion pv = {.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION};
    F.query(&x->ctx, &pv.header);
    rt_log("fsr: FSR %s context at %ux%u -> %ux%u, provider %s", ml ? "4" : "3.1", f->w, f->h, f->ow, f->oh,
           pv.versionName ? pv.versionName : "?");
    return x;
}

void vk_ffx_destroy(void *ctx) {
    Ctx *x = ctx;
    if (!x) return;
    if (x->ctx) F.destroy(&x->ctx, NULL);
    free(x);
}

int vk_ffx_ctx_ml(const void *ctx) { return ctx && ((const Ctx *)ctx)->ml; }

static int depth_has_stencil(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

// The three inputs between GENERAL and shader-read (to_read) for AMD's runtime, through the recorder.
static void inputs_layout(VkCommandBuffer cb, const VkFfxFrame *f, int to_read) {
    VkImageLayout a = to_read ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkImageLayout b = to_read ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    VkImage im[3] = {f->color, f->depth, f->mv};
    VkImageSubresourceRange sr[3] = {
        {VK_IMAGE_ASPECT_COLOR_BIT, f->color_level, 1, f->color_layer, 1},
        {VK_IMAGE_ASPECT_DEPTH_BIT | (depth_has_stencil(f->depth_fmt) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0),
         f->depth_level, 1, f->depth_layer, 1},
        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageMemoryBarrier2 ib[3];
    for (int i = 0; i < 3; i++)
        ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = a, .newLayout = b,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[i],
                                        .subresourceRange = sr[i]};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 3,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static struct FfxApiResource res(VkImage im, uint32_t fmt, uint32_t w, uint32_t h, uint32_t usage, uint32_t state) {
    struct FfxApiResource r;
    memset(&r, 0, sizeof r);
    r.resource = (void *)(uintptr_t)im;
    r.state = state;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = fmt;
    r.description.width = w, r.description.height = h, r.description.depth = 1, r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = usage;
    return r;
}

static uint32_t color_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;
    default: return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

int vk_ffx_dispatch(VkCommandBuffer cb, void **slot, const VkFfxFrame *f) {
    if (!vk_ffx_available()) return 0;
    // AMD's runtime addresses a whole image: the scene and its depth must be the first level and layer, the depth a
    // float one (D24 would be read as integers). MLB's scene is (vk_dlaa.c); anything else is left to the game.
    uint32_t cf = color_format(f->color_fmt);
    if (cf == FFX_API_SURFACE_FORMAT_UNKNOWN || f->color_level || f->color_layer || f->depth_level ||
        f->depth_layer || (f->depth_fmt != VK_FORMAT_D32_SFLOAT && f->depth_fmt != VK_FORMAT_D32_SFLOAT_S8_UINT)) {
        static int n;
        if (n++ < 5)
            rt_log("fsr: scene not usable (format %d level %u layer %u, depth format %d level %u layer %u): skipped",
                   (int)f->color_fmt, f->color_level, f->color_layer, (int)f->depth_fmt, f->depth_level,
                   f->depth_layer);
        return 0;
    }
    Ctx *x = *slot;
    if (!x) {
        int ml = vk_ffx_ml_wanted();
        x = ctx_make(f, ml);
        if (!x && ml) {
            F.failed4 = 1;
            rt_log("fsr: FSR 4 did not start on this GPU: FSR 3.1 from now on");
            x = F.id31 ? ctx_make(f, 0) : NULL;
        }
        if (!x) {
            rt_log("fsr: no FSR context could be made: FSR off for this run");
            F.failed = 1;
            return 0;
        }
        *slot = x;
    }
    uint32_t zusage = FFX_API_RESOURCE_USAGE_DEPTHTARGET |
                      (depth_has_stencil(f->depth_fmt) ? FFX_API_RESOURCE_USAGE_STENCILTARGET : 0);
    static float sharp = -1;
    static unsigned seen = ~0u;
    if (cfg_version() != seen) {   // MLB_FSR_SHARPNESS, live: FSR's own RCAS after the upscale, 0..100
        seen = cfg_version();
        sharp = (float)atof(cfg_value("FSR_SHARPNESS")) / 100.0f;
        if (sharp < 0) sharp = 0;
        if (sharp > 1) sharp = 1;
    }
    struct ffxDispatchDescUpscale d = {
        .header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE, .commandList = cb,
        .color = res(f->color, cf, f->w, f->h, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ),
        .depth = res(f->depth, FFX_API_SURFACE_FORMAT_R32_FLOAT, f->w, f->h, zusage, FFX_API_RESOURCE_STATE_COMPUTE_READ),
        .motionVectors = res(f->mv, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, f->w, f->h, FFX_API_RESOURCE_USAGE_READ_ONLY,
                             FFX_API_RESOURCE_STATE_COMPUTE_READ),
        .output = res(f->out, FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT, f->ow, f->oh, FFX_API_RESOURCE_USAGE_UAV,
                      FFX_API_RESOURCE_STATE_UNORDERED_ACCESS),
        .jitterOffset = {f->jx, f->jy}, .motionVectorScale = {1.0f, 1.0f},   // render pixels already
        .renderSize = {f->w, f->h}, .upscaleSize = {f->ow, f->oh},
        .enableSharpening = sharp > 0, .sharpness = sharp,
        .frameTimeDelta = f->dt_ms, .preExposure = 1.0f, .reset = f->reset != 0,
        .cameraNear = f->near_z, .cameraFar = f->far_z, .cameraFovAngleVertical = f->fov_y,
        .viewSpaceToMetersFactor = 1.0f, .flags = 0};
    inputs_layout(cb, f, 1);
    rec_drain();   // AMD's runtime records into cb itself: the recorder must have made every call so far
    ffxReturnCode_t r = F.dispatch(&x->ctx, &d.header);
    inputs_layout(cb, f, 0);
    if (r != FFX_API_RETURN_OK) {
        rt_log("fsr: FSR %s dispatch failed (%u): FSR off for this run", x->ml ? "4" : "3.1", r);
        F.failed = 1;
        return 0;
    }
    F.last_ml = x->ml;
    return 1;
}
