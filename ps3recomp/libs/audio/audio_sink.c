#include "audio_sink.h"

#include "cellAudio.h"
#include <ps3emu/host_platform.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#if defined(PS3RECOMP_AUDIO_BACKEND_SDL3)

#include <ps3emu/host_sdl.h>
#include <SDL3/SDL.h>

#define AUDIO_FRAME_BYTES (2u * (uint32_t)sizeof(float))
/* The device pulls a whole ALSA period at once (four 256-frame blocks on the
 * Pi), so a four-block queue has exactly zero margin for scheduler jitter.
 * Each extra block is 5.33 ms of output latency; TAIKO_AUDIO_OFFSET_MS
 * compensates the song against it. */
#define SDL_DEFAULT_PREBUFFER_BLOCKS 6u
#define SDL_MAX_PREBUFFER_BLOCKS 32u

static SDL_AudioStream* s_sdl_stream;
static uint32_t s_sdl_submitted_blocks;
static uint32_t s_sdl_device_buffer_frames;
static uint32_t s_sdl_prebuffer_blocks = SDL_DEFAULT_PREBUFFER_BLOCKS;
static atomic_int s_sdl_resumed;
static atomic_ullong s_sdl_starvation_events;
static atomic_ullong s_sdl_starvation_frames;

static void SDLCALL sdl_audio_get_callback(void* userdata,
                                           SDL_AudioStream* stream,
                                           int additional_amount,
                                           int total_amount)
{
    (void)userdata;
    (void)stream;
    (void)total_amount;
    /* SDL calls this immediately before its playback device obtains data.
     * If additional input is required and we do not supply it synchronously,
     * that device pull will be short and SDL will pad it with silence.  Do not
     * log on the real-time audio thread; publish counters for cellAudio's
     * once-per-second trace instead. */
    if (atomic_load_explicit(&s_sdl_resumed, memory_order_relaxed) &&
        additional_amount > 0) {
        atomic_fetch_add_explicit(&s_sdl_starvation_events, 1,
                                  memory_order_relaxed);
        atomic_fetch_add_explicit(
            &s_sdl_starvation_frames,
            (unsigned long long)additional_amount / AUDIO_FRAME_BYTES,
            memory_order_relaxed);
    }
}

const char* audio_sink_name(void) { return "sdl3"; }

int audio_sink_init(void)
{
    if (getenv("PS3RECOMP_NULL_AUDIO")) return AUDIO_SINK_INIT_NULL_CLOCK;
    if (!ps3_host_sdl_audio_available()) {
        fprintf(stderr, "[cellAudio] SDL audio subsystem is unavailable\n");
        return AUDIO_SINK_INIT_FAILED;
    }

    s_sdl_prebuffer_blocks = SDL_DEFAULT_PREBUFFER_BLOCKS;
    const char* prebuffer_text = getenv("TAIKO_AUDIO_PREBUFFER_BLOCKS");
    if (prebuffer_text && *prebuffer_text) {
        const unsigned long parsed = strtoul(prebuffer_text, NULL, 0);
        if (parsed >= 2u && parsed <= SDL_MAX_PREBUFFER_BLOCKS)
            s_sdl_prebuffer_blocks = (uint32_t)parsed;
    }

    const SDL_AudioSpec spec = {
        .format = SDL_AUDIO_F32,
        .channels = 2,
        .freq = CELL_AUDIO_SAMPLE_RATE,
    };
    s_sdl_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
        sdl_audio_get_callback, NULL);
    if (!s_sdl_stream) {
        fprintf(stderr, "[cellAudio] SDL_OpenAudioDeviceStream failed: %s\n",
                SDL_GetError());
        return AUDIO_SINK_INIT_FAILED;
    }
    s_sdl_submitted_blocks = 0;
    s_sdl_device_buffer_frames = 0;
    atomic_store_explicit(&s_sdl_resumed, 0, memory_order_relaxed);
    atomic_store_explicit(&s_sdl_starvation_events, 0, memory_order_relaxed);
    atomic_store_explicit(&s_sdl_starvation_frames, 0, memory_order_relaxed);
    SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(s_sdl_stream);
    SDL_AudioSpec device_spec = {0};
    int device_frames = 0;
    if (SDL_GetAudioDeviceFormat(device, &device_spec, &device_frames) &&
        device_frames > 0)
        s_sdl_device_buffer_frames = (uint32_t)device_frames;
    const char* driver = SDL_GetCurrentAudioDriver();
    const char* device_name = SDL_GetAudioDeviceName(device);
    fprintf(stderr,
            "[cellAudio] SDL3 sink opened (driver=%s device=%s, "
            "48000 Hz stereo float, %u-block prebuffer, "
            "device=%d Hz/%u frames=%.2f ms)\n",
            driver ? driver : "unknown",
            device_name ? device_name : "unknown",
            s_sdl_prebuffer_blocks,
            device_spec.freq, s_sdl_device_buffer_frames,
            device_spec.freq > 0
                ? 1000.0 * s_sdl_device_buffer_frames / device_spec.freq : 0.0);
    return AUDIO_SINK_INIT_OK;
}

void audio_sink_shutdown(void)
{
    if (s_sdl_stream) SDL_DestroyAudioStream(s_sdl_stream);
    s_sdl_stream = NULL;
    s_sdl_submitted_blocks = 0;
    s_sdl_device_buffer_frames = 0;
    atomic_store_explicit(&s_sdl_resumed, 0, memory_order_relaxed);
}

uint32_t audio_sink_queued_frames(void)
{
    if (!s_sdl_stream) return 0;
    int bytes = SDL_GetAudioStreamQueued(s_sdl_stream);
    return bytes > 0 ? (uint32_t)bytes / AUDIO_FRAME_BYTES : 0;
}

uint32_t audio_sink_device_buffer_frames(void)
{
    return s_sdl_device_buffer_frames;
}

uint64_t audio_sink_starvation_events(void)
{
    return atomic_load_explicit(&s_sdl_starvation_events,
                                memory_order_relaxed);
}

uint64_t audio_sink_starvation_frames(void)
{
    return atomic_load_explicit(&s_sdl_starvation_frames,
                                memory_order_relaxed);
}

int audio_sink_wait_for_block(uint32_t frames, const volatile int* running)
{
    (void)frames;
    if (!s_sdl_stream) return 0;
    if (!atomic_load_explicit(&s_sdl_resumed, memory_order_relaxed)) return 1;

    const uint64_t deadline = ps3_host_monotonic_ns() + 1000000000u;
    while (*running) {
        int bytes = SDL_GetAudioStreamQueued(s_sdl_stream);
        if (bytes < 0) {
            fprintf(stderr, "[cellAudio] SDL_GetAudioStreamQueued failed: %s\n",
                    SDL_GetError());
            return 0;
        }
        uint32_t queued = (uint32_t)bytes / AUDIO_FRAME_BYTES;
        if (queued <=
            (s_sdl_prebuffer_blocks - 1u) * CELL_AUDIO_BLOCK_SAMPLES)
            return 1;
        if (ps3_host_monotonic_ns() >= deadline) {
            fprintf(stderr, "[cellAudio] SDL audio queue stopped consuming\n");
            return 0;
        }
        ps3_host_sleep_ms(1);
    }
    return 0;
}

int audio_sink_submit(const float* stereo_samples, uint32_t frames)
{
    if (!s_sdl_stream) return 0;
    const int bytes = (int)(frames * AUDIO_FRAME_BYTES);
    if (!SDL_PutAudioStreamData(s_sdl_stream, stereo_samples, bytes)) {
        fprintf(stderr, "[cellAudio] SDL_PutAudioStreamData failed: %s\n",
                SDL_GetError());
        return 0;
    }
    ++s_sdl_submitted_blocks;
    if (!atomic_load_explicit(&s_sdl_resumed, memory_order_relaxed) &&
        s_sdl_submitted_blocks >= s_sdl_prebuffer_blocks) {
        if (!SDL_ResumeAudioStreamDevice(s_sdl_stream)) {
            fprintf(stderr, "[cellAudio] SDL_ResumeAudioStreamDevice failed: %s\n",
                    SDL_GetError());
            return 0;
        }
        atomic_store_explicit(&s_sdl_resumed, 1, memory_order_relaxed);
    }
    return 1;
}

#elif defined(PS3RECOMP_AUDIO_BACKEND_WASAPI)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#define DEFINE_AUDIO_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8) \
    const GUID name = { l, w1, w2, { b1, b2, b3, b4, b5, b6, b7, b8 } }
DEFINE_AUDIO_GUID(ps3r_CLSID_MMDeviceEnumerator, 0xBCDE0395,0xE52F,0x467C,0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E);
DEFINE_AUDIO_GUID(ps3r_IID_IMMDeviceEnumerator,  0xA95664D2,0x9614,0x4F35,0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6);
DEFINE_AUDIO_GUID(ps3r_IID_IAudioClient,         0x1CB9AD4C,0xDBFA,0x4c32,0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2);
DEFINE_AUDIO_GUID(ps3r_IID_IAudioRenderClient,   0xF294ACFC,0x3146,0x4483,0xA7,0xBF,0xAD,0xDC,0xA7,0xC2,0x60,0xE2);
#define IID_IMMDeviceEnumerator  ps3r_IID_IMMDeviceEnumerator
#define CLSID_MMDeviceEnumerator ps3r_CLSID_MMDeviceEnumerator
#define IID_IAudioClient         ps3r_IID_IAudioClient
#define IID_IAudioRenderClient   ps3r_IID_IAudioRenderClient

static IAudioClient* s_client;
static IAudioRenderClient* s_render;
static HANDLE s_event;
static UINT32 s_buffer_frames;

const char* audio_sink_name(void) { return "wasapi"; }

int audio_sink_init(void)
{
    if (getenv("PS3RECOMP_NULL_AUDIO")) return AUDIO_SINK_INIT_NULL_CLOCK;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) return AUDIO_SINK_INIT_FAILED;

    IMMDeviceEnumerator* enumerator = NULL;
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void**)&enumerator);
    if (FAILED(hr)) return AUDIO_SINK_INIT_FAILED;
    IMMDevice* device = NULL;
    hr = enumerator->lpVtbl->GetDefaultAudioEndpoint(enumerator, eRender, eConsole, &device);
    enumerator->lpVtbl->Release(enumerator);
    if (FAILED(hr)) return AUDIO_SINK_INIT_FAILED;
    hr = device->lpVtbl->Activate(device, &IID_IAudioClient, CLSCTX_ALL,
                                  NULL, (void**)&s_client);
    device->lpVtbl->Release(device);
    if (FAILED(hr)) return AUDIO_SINK_INIT_FAILED;

    WAVEFORMATEX format = {
        .wFormatTag = WAVE_FORMAT_IEEE_FLOAT,
        .nChannels = 2,
        .nSamplesPerSec = CELL_AUDIO_SAMPLE_RATE,
        .nAvgBytesPerSec = CELL_AUDIO_SAMPLE_RATE * 2u * sizeof(float),
        .nBlockAlign = 2u * sizeof(float),
        .wBitsPerSample = 32,
        .cbSize = 0,
    };
    s_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    hr = s_client->lpVtbl->Initialize(s_client, AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, &format, NULL);
    if (FAILED(hr)) {
        hr = s_client->lpVtbl->Initialize(s_client, AUDCLNT_SHAREMODE_SHARED,
                                           0, 200000, 0, &format, NULL);
        if (FAILED(hr)) return AUDIO_SINK_INIT_FAILED;
        CloseHandle(s_event);
        s_event = NULL;
    } else {
        s_client->lpVtbl->SetEventHandle(s_client, s_event);
    }
    s_client->lpVtbl->GetBufferSize(s_client, &s_buffer_frames);
    hr = s_client->lpVtbl->GetService(s_client, &IID_IAudioRenderClient,
                                      (void**)&s_render);
    if (FAILED(hr)) return AUDIO_SINK_INIT_FAILED;
    hr = s_client->lpVtbl->Start(s_client);
    if (FAILED(hr)) return AUDIO_SINK_INIT_FAILED;
    fprintf(stderr, "[cellAudio] WASAPI sink opened (%u-frame endpoint buffer)\n",
            (unsigned)s_buffer_frames);
    return AUDIO_SINK_INIT_OK;
}

void audio_sink_shutdown(void)
{
    if (s_client) s_client->lpVtbl->Stop(s_client);
    if (s_render) s_render->lpVtbl->Release(s_render);
    if (s_client) s_client->lpVtbl->Release(s_client);
    if (s_event) CloseHandle(s_event);
    s_render = NULL;
    s_client = NULL;
    s_event = NULL;
    s_buffer_frames = 0;
}

uint32_t audio_sink_queued_frames(void)
{
    if (!s_client) return 0;
    UINT32 padding = 0;
    return SUCCEEDED(s_client->lpVtbl->GetCurrentPadding(s_client, &padding))
        ? padding : 0;
}

uint32_t audio_sink_device_buffer_frames(void)
{
    return s_buffer_frames;
}

uint64_t audio_sink_starvation_events(void) { return 0; }
uint64_t audio_sink_starvation_frames(void) { return 0; }

int audio_sink_wait_for_block(uint32_t frames, const volatile int* running)
{
    if (!s_client || !s_render) return 0;
    const uint64_t deadline = ps3_host_monotonic_ns() + 1000000000u;
    while (*running) {
        UINT32 padding = 0;
        if (FAILED(s_client->lpVtbl->GetCurrentPadding(s_client, &padding))) return 0;
        if (s_buffer_frames - padding >= frames) return 1;
        if (ps3_host_monotonic_ns() >= deadline) return 0;
        if (s_event) WaitForSingleObject(s_event, 10);
        else Sleep(1);
    }
    return 0;
}

int audio_sink_submit(const float* stereo_samples, uint32_t frames)
{
    if (!s_render) return 0;
    BYTE* buffer = NULL;
    HRESULT hr = s_render->lpVtbl->GetBuffer(s_render, frames, &buffer);
    if (FAILED(hr) || !buffer) return 0;
    memcpy(buffer, stereo_samples, frames * 2u * sizeof(float));
    return SUCCEEDED(s_render->lpVtbl->ReleaseBuffer(s_render, frames, 0));
}

#elif defined(PS3RECOMP_AUDIO_BACKEND_NULL)

const char* audio_sink_name(void) { return "null"; }
int audio_sink_init(void) { return AUDIO_SINK_INIT_NULL_CLOCK; }
void audio_sink_shutdown(void) {}
int audio_sink_wait_for_block(uint32_t frames, const volatile int* running)
{ (void)frames; return *running != 0; }
int audio_sink_submit(const float* samples, uint32_t frames)
{ (void)samples; (void)frames; return 1; }
uint32_t audio_sink_queued_frames(void) { return 0; }
uint32_t audio_sink_device_buffer_frames(void) { return 0; }
uint64_t audio_sink_starvation_events(void) { return 0; }
uint64_t audio_sink_starvation_frames(void) { return 0; }

#else
#error "No supported ps3recomp audio backend selected"
#endif
