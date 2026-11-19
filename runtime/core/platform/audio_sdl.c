// Host audio output on SDL3 (audio_out.h), for Linux: the WASAPI output's counterpart (audio_win32.c). SDL picks
// PipeWire, PulseAudio or ALSA, converts our float32 stream to the device's format, and calls back on its own audio
// thread for more; the callback pulls from the mixer. The SDL3 audio stream is the one TotK Recomp's audio_mixer.c
// @ ae30a61 (MIT) opens; here it sits behind audio_out.h so the mixer is the same on every OS.
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_out.h"

struct AudioOut {
    AudioPullFn pull;
    void *user;
    int channels;
    SDL_AudioStream *stream;
};

static int audio_init(void) {
    static int ok = -1;
    if (ok < 0) ok = SDL_InitSubSystem(SDL_INIT_AUDIO) ? 1 : 0;   // the first call is on one thread (the mixer's open)
    return ok;
}

int audio_out_default_channels(void) {
    SDL_AudioSpec dev = {0};
    if (!audio_init() || !SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &dev, NULL)) return 0;
    return dev.channels;
}

// SDL asks for `additional` more bytes; the pull fills them in pieces of at most 1024 frames.
static void SDLCALL sdl_pull(void *user, SDL_AudioStream *stream, int additional, int total) {
    (void)total;
    AudioOut *o = user;
    float tmp[8 * 1024];
    const int frame_bytes = o->channels * (int)sizeof(float);
    while (additional >= frame_bytes) {
        uint32_t frames = (uint32_t)(additional / frame_bytes);
        if (frames > 1024) frames = 1024;
        o->pull(o->user, tmp, frames);
        if (!SDL_PutAudioStreamData(stream, tmp, (int)frames * frame_bytes)) break;
        additional -= (int)frames * frame_bytes;
    }
}

AudioOut *audio_out_open(int rate, int channels, AudioPullFn pull, void *user, int *period, char *name, size_t nname,
                         char *err, size_t nerr) {
    if (channels < 1 || channels > 8) {
        snprintf(err, nerr, "%d channels", channels);
        return NULL;
    }
    if (!audio_init()) {
        snprintf(err, nerr, "SDL audio init failed: %s", SDL_GetError());
        return NULL;
    }
    AudioOut *o = calloc(1, sizeof *o);
    if (!o) {
        snprintf(err, nerr, "out of memory");
        return NULL;
    }
    o->pull = pull, o->user = user, o->channels = channels;
    SDL_AudioSpec spec = {SDL_AUDIO_F32, channels, rate};
    o->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, sdl_pull, o);
    if (!o->stream) {
        snprintf(err, nerr, "no output device: %s", SDL_GetError());
        free(o);
        return NULL;
    }
    if (!SDL_ResumeAudioStreamDevice(o->stream)) {
        snprintf(err, nerr, "starting the output failed: %s", SDL_GetError());
        SDL_DestroyAudioStream(o->stream);
        free(o);
        return NULL;
    }
    SDL_AudioSpec dev = {0};
    int frames = 0;
    if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(o->stream), &dev, &frames) || frames <= 0) frames = 0;
    *period = frames;
    const char *n = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(o->stream));
    snprintf(name, nname, "%s (SDL %s)", n ? n : "?", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
    return o;
}

void audio_out_close(AudioOut *o) {
    if (!o) return;
    SDL_DestroyAudioStream(o->stream);   // returns after the callback has finished
    free(o);
}
