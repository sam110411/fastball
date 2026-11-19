// The software keyboard's front end with the SDL3 window (swkbd.h), for Linux. After TotK Recomp runtime/swkbd_sdl.c
// @ ae30a61 (MIT, LICENSE.totk-recomp): until a text field of our own exists (an ImGui entry in the overlay, which
// SDL_StartTextInput can pair with the Steam Deck's on-screen keyboard), the entry takes the game's starting text at
// once (a Road to the Show player's default name, for one). MLB_SWKBD_TEST=cancel stands for Cancel pressed.
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "swkbd.h"

int text_entry_run(const SwkbdRequest *req, uint16_t *out, size_t cap, size_t *n) {
    const char *test = cfg_get("MLB_SWKBD_TEST");
    if (test && !strcmp(test, "cancel")) {
        *n = 0;
        return 0;
    }
    size_t k = 0;
    while (k < cap && req->initial[k]) k++;
    memcpy(out, req->initial, k * 2);
    *n = k;
    rt_log("swkbd: no text entry window with SDL yet: the game's starting text is taken (%zu characters)", k);
    return k > 0;
}
