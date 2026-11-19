// fastball, our own: RenderDoc captures of whole game frames (replaces the gfx_stubs.c no-ops of TotK's rdoc.c).
//
// Under RenderDoc (renderdoc.dll injected, e.g. tools/renderdoc.cmd), its own capture keys capture from one host
// present to the next. In menus the game presents some frames without drawing anything new, so such a capture held only
// the present's blit (2 of 3 captures of a glitched menu). Here F12 is taken over through the in-application API: the
// capture starts at a present and ends at the first present after the game submitted work, so it always holds a game
// frame (and the presents with nothing in between). Start and end come after the present was handed to the present
// thread, so the frame's submissions are inside; its blit may fall in either capture. Outside RenderDoc nothing runs.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "cfg.h"
#include "host.h"
#include "keys.h"
#include "vk.h"
#include "renderdoc_app.h"

uint64_t vk_rdoc_game_submits;   // vk_backend.c submit_sync: submissions that carried game command buffers

#ifdef _WIN32
static RENDERDOC_API_1_6_0 *api;
static int capturing, presents_in;
static uint64_t start_submits, start_frame;

void rdoc_init(void) {
    HMODULE m = GetModuleHandleA("renderdoc.dll");
    if (!m || api) return;
    pRENDERDOC_GetAPI get = (pRENDERDOC_GetAPI)(void *)GetProcAddress(m, "RENDERDOC_GetAPI");
    if (!get || get(eRENDERDOC_API_Version_1_6_0, (void **)&api) != 1 || !api) {
        api = NULL;
        rt_log("rdoc: renderdoc.dll is loaded but its API 1.6.0 is not available: RenderDoc's own F12 stays");
        return;
    }
    api->SetCaptureKeys(NULL, 0);   // F12 is ours: rdoc_after_present
    int ma, mi, pa;
    api->GetAPIVersion(&ma, &mi, &pa);
    rt_log("rdoc: RenderDoc API %d.%d.%d: F12 captures the next game frame (from a present to the first present after "
           "game work)", ma, mi, pa);
}

int rdoc_active(void) { return api != NULL; }

static int f12_pressed(void) {
    static int was;
    int down = wsys_key_held(KEY_F12);
    int edge = down && !was;
    was = down;
    return edge;
}

// After the present of `frame` was handed over and submitted (vk_backend.c present_image, the replay thread).
void rdoc_after_present(uint64_t frame) {
    if (!api) return;
    uint64_t subs = __atomic_load_n(&vk_rdoc_game_submits, __ATOMIC_RELAXED);
    if (!capturing) {
        // MLB_RDOC_FRAME=<frame>: also a capture at that frame without a key (repeatable captures, tests)
        static long at = -2;
        if (at == -2) {
            const char *e = cfg_get("MLB_RDOC_FRAME");
            at = e && *e ? atol(e) : -1;
        }
        int key = f12_pressed();
        if (!key && !(at >= 0 && frame >= (uint64_t)at)) return;
        if (!key) at = -1;
        // An API capture is saved as <template>_capture.rdc, the same name each time: <template>_frame<N> instead
        static char base[900];
        if (!base[0]) {
            const char *t = api->GetCaptureFilePathTemplate();
            snprintf(base, sizeof base, "%s", t && *t ? t : "mlb");
        }
        char name[960];
        snprintf(name, sizeof name, "%s_frame%llu", base, (unsigned long long)frame);
        api->SetCaptureFilePathTemplate(name);
        vk_queue_lock();   // RenderDoc submits its own work here too (see EndFrameCapture below)
        api->StartFrameCapture(NULL, NULL);
        vk_queue_unlock();
        capturing = 1, presents_in = 0, start_submits = subs, start_frame = frame;
        return;
    }
    presents_in++;
    // At least one present with game work behind it; 30 presents without any (a stall) end it as it is.
    if (subs == start_submits && presents_in < 30) return;
    // Under the queue lock, which every port submission and present takes. RenderDoc 1.43's EndFrameCapture
    // (vk_core.cpp) switches back to background capturing first, then waits for the device, reads back the backbuffer
    // and writes the file for seconds, submitting on gfx_queue itself. Unlocked, the present thread's next
    // vkQueuePresentKHR took RenderDoc's overlay path (HandlePresent: DoPipelineBarrier, a submit) on the same queue at
    // the same time: two threads on one VkQueue, and nvoglv64.dll crashed writing 0x4000000001 (a capture in a match,
    // 1.95 GB written). The present thread now waits until the capture is saved.
    vk_queue_lock();
    uint32_t ok = api->EndFrameCapture(NULL, NULL);
    vk_queue_unlock();
    capturing = 0;
    char path[1024] = "";
    uint32_t len = sizeof path;
    uint32_t n = api->GetNumCaptures();
    if (ok && n) api->GetCapture(n - 1, path, &len, NULL);
    rt_log("rdoc: capture of frames %llu-%llu (%llu game submissions, %d presents): %s", (unsigned long long)start_frame,
           (unsigned long long)frame, (unsigned long long)(subs - start_submits), presents_in,
           ok ? path : "failed (see the RenderDoc log)");
}
#else
void rdoc_init(void) {}
int rdoc_active(void) { return 0; }
void rdoc_after_present(uint64_t frame) { (void)frame; }
#endif
