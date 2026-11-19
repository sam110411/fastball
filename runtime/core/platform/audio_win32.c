// Host audio output on WASAPI (audio_out.h). fastball, our own: it takes the place of the SDL3 audio stream in
// TotK Recomp's audio_mixer.c, so the runtime needs no SDL3.dll.
//
// One thread owns everything COM: it opens the default render endpoint in shared mode with an event-driven float32
// stream (WASAPI converts rate and format to the mix format: AUTOCONVERTPCM), fills the whole free part of the
// endpoint buffer from the pull callback on every period event, and releases the device when told to stop. COM stays
// on that thread, so callers never need an apartment.
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_out.h"

// The GUIDs are defined here rather than taken from uuid.lib, which does not carry all of them in every SDK.
static const GUID clsid_enumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID iid_enumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID iid_client = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID iid_render = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const GUID subtype_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const PROPERTYKEY key_friendly_name = {{0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}},
                                              14};

#define BUFFER_HNS 200000   // 20 ms endpoint buffer: the mixer's ring holds the rest of the queue

struct AudioOut {
    AudioPullFn pull;
    void *user;
    int rate, channels;
    HANDLE thread, ready, wake;
    volatile LONG stop;
    int ok, period;        // set by the thread before `ready`
    char name[128], err[160];
};

static IMMDevice *default_device(IMMDeviceEnumerator **en, HRESULT *hr) {
    *en = NULL;
    IMMDevice *dev = NULL;
    *hr = CoCreateInstance(&clsid_enumerator, NULL, CLSCTX_ALL, &iid_enumerator, (void **)en);
    if (SUCCEEDED(*hr)) *hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(*en, eRender, eConsole, &dev);
    return SUCCEEDED(*hr) ? dev : NULL;
}

static void device_name(IMMDevice *dev, char *out, size_t n) {
    snprintf(out, n, "?");
    IPropertyStore *ps = NULL;
    if (FAILED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) return;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(IPropertyStore_GetValue(ps, &key_friendly_name, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
        WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, out, (int)n, NULL, NULL);
    PropVariantClear(&v);
    IPropertyStore_Release(ps);
}

int audio_out_default_channels(void) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int com = SUCCEEDED(hr), n = 0;
    IMMDeviceEnumerator *en;
    IMMDevice *dev = default_device(&en, &hr);
    IAudioClient *client = NULL;
    WAVEFORMATEX *mix = NULL;
    if (dev && SUCCEEDED(IMMDevice_Activate(dev, &iid_client, CLSCTX_ALL, NULL, (void **)&client)) &&
        SUCCEEDED(IAudioClient_GetMixFormat(client, &mix)))
        n = mix->nChannels;
    if (mix) CoTaskMemFree(mix);
    if (client) IAudioClient_Release(client);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    if (com) CoUninitialize();
    return n;
}

static DWORD WINAPI out_thread(void *p) {
    AudioOut *o = p;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    int com = SUCCEEDED(hr);
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioClient *client = NULL;
    IAudioRenderClient *render = NULL;
    UINT32 frames = 0;
    const char *step = "CoInitializeEx";
    if (!com) goto done;
    step = "the default playback device";
    if (!(dev = default_device(&en, &hr))) goto done;
    device_name(dev, o->name, sizeof o->name);
    step = "IAudioClient";
    if (FAILED(hr = IMMDevice_Activate(dev, &iid_client, CLSCTX_ALL, NULL, (void **)&client))) goto done;

    WAVEFORMATEXTENSIBLE f = {0};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = (WORD)o->channels;
    f.Format.nSamplesPerSec = (DWORD)o->rate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = (WORD)(o->channels * 4);
    f.Format.nAvgBytesPerSec = (DWORD)o->rate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof f - sizeof f.Format;
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = o->channels == 6 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                                          SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
                                       : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    f.SubFormat = subtype_float;
    step = "IAudioClient::Initialize";
    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
                                 AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                     AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 BUFFER_HNS, 0, &f.Format, NULL);
    if (FAILED(hr)) goto done;
    step = "the endpoint buffer";
    if (FAILED(hr = IAudioClient_SetEventHandle(client, o->wake))) goto done;
    if (FAILED(hr = IAudioClient_GetBufferSize(client, &frames))) goto done;
    if (FAILED(hr = IAudioClient_GetService(client, &iid_render, (void **)&render))) goto done;
    REFERENCE_TIME period_hns = 0;
    if (FAILED(IAudioClient_GetDevicePeriod(client, &period_hns, NULL)) || period_hns <= 0) period_hns = 100000;
    o->period = (int)(period_hns * o->rate / 10000000);
    BYTE *data;
    step = "the first fill";
    if (FAILED(hr = IAudioRenderClient_GetBuffer(render, frames, &data))) goto done;
    memset(data, 0, (size_t)frames * f.Format.nBlockAlign);
    IAudioRenderClient_ReleaseBuffer(render, frames, AUDCLNT_BUFFERFLAGS_SILENT);
    step = "IAudioClient::Start";
    if (FAILED(hr = IAudioClient_Start(client))) goto done;
    o->ok = 1;

done:
    if (!o->ok) snprintf(o->err, sizeof o->err, "%s failed (%#lx)", step, (unsigned long)hr);
    SetEvent(o->ready);   // o may only be touched by this thread from here on, until audio_out_close joins it
    if (o->ok) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        while (!o->stop) {
            WaitForSingleObject(o->wake, 100);
            if (o->stop) break;
            UINT32 pad = 0;
            if (FAILED(IAudioClient_GetCurrentPadding(client, &pad)) || pad >= frames) continue;
            UINT32 n = frames - pad;
            if (FAILED(IAudioRenderClient_GetBuffer(render, n, &data))) continue;   // e.g. the device went away
            o->pull(o->user, (float *)data, n);
            IAudioRenderClient_ReleaseBuffer(render, n, 0);
        }
        IAudioClient_Stop(client);
    }
    if (render) IAudioRenderClient_Release(render);
    if (client) IAudioClient_Release(client);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    if (com) CoUninitialize();
    return 0;
}

AudioOut *audio_out_open(int rate, int channels, AudioPullFn pull, void *user, int *period, char *name, size_t nname,
                         char *err, size_t nerr) {
    AudioOut *o = calloc(1, sizeof *o);
    if (!o) {
        snprintf(err, nerr, "out of memory");
        return NULL;
    }
    *o = (AudioOut){.pull = pull, .user = user, .rate = rate, .channels = channels};
    o->ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    o->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (o->ready && o->wake) o->thread = CreateThread(NULL, 0, out_thread, o, 0, NULL);
    if (!o->thread) {
        snprintf(err, nerr, "cannot start the output thread");
    } else {
        WaitForSingleObject(o->ready, INFINITE);
        if (o->ok) {
            *period = o->period;
            snprintf(name, nname, "%s (WASAPI)", o->name);
            return o;
        }
        snprintf(err, nerr, "%s", o->err);
        WaitForSingleObject(o->thread, INFINITE);
        CloseHandle(o->thread);
    }
    if (o->ready) CloseHandle(o->ready);
    if (o->wake) CloseHandle(o->wake);
    free(o);
    return NULL;
}

void audio_out_close(AudioOut *o) {
    if (!o) return;
    InterlockedExchange(&o->stop, 1);
    SetEvent(o->wake);
    WaitForSingleObject(o->thread, INFINITE);
    CloseHandle(o->thread);
    CloseHandle(o->ready);
    CloseHandle(o->wake);
    free(o);
}
