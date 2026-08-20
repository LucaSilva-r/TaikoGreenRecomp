/*
 * ps3recomp - cellAvconfExt HLE implementation
 *
 * Reports standard LPCM stereo 48kHz as available audio output.
 * Games query this to decide audio format before opening cellAudio ports.
 */

#include "cellAvconfExt.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

extern void vm_write8(uint64_t addr, uint8_t value);
extern void vm_write32(uint64_t addr, uint32_t value);

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static float s_gamma = 1.0f;

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioOutGetSoundAvailability(u32 audioOut, u32 type, u32 fs, u32 option)
{
    (void)audioOut;
    (void)option;

    /* Report 2-channel LPCM at 48kHz as always available */
    if (type == CELL_AUDIO_OUT_CODING_TYPE_LPCM && (fs & CELL_AUDIO_OUT_FS_48KHZ))
        return CELL_AUDIO_OUT_CHNUM_2;

    /* 6-channel (5.1) LPCM also available */
    if (type == CELL_AUDIO_OUT_CODING_TYPE_LPCM)
        return CELL_AUDIO_OUT_CHNUM_2;

    return 0; /* not available */
}

s32 cellAudioOutGetSoundAvailability2(u32 audioOut, u32 type, u32 fs, u32 ch, u32 option)
{
    (void)audioOut;
    (void)option;

    if (type == CELL_AUDIO_OUT_CODING_TYPE_LPCM &&
        (fs & CELL_AUDIO_OUT_FS_48KHZ) &&
        (ch == CELL_AUDIO_OUT_CHNUM_2 || ch == CELL_AUDIO_OUT_CHNUM_6 || ch == CELL_AUDIO_OUT_CHNUM_8))
        return 1; /* available */

    return 0;
}

s32 cellAudioOutGetDeviceInfo(u32 audioOut, u32 deviceIndex,
                               CellAudioOutDeviceInfo* info)
{
    (void)audioOut;
    (void)deviceIndex;

    printf("[cellAvconfExt] AudioOutGetDeviceInfo(out=%u, dev=%u)\n",
           audioOut, deviceIndex);

    if (!info)
        return CELL_EINVAL;

    uint32_t info_ea = (uint32_t)(uintptr_t)info;
    for (uint32_t i = 0; i < sizeof(CellAudioOutDeviceInfo); i++)
        vm_write8(info_ea + i, 0);

    /* Report one available mode: LPCM stereo 48kHz */
    vm_write8(info_ea + 0, 0); /* HDMI */
    vm_write8(info_ea + 1, 1);
    vm_write8(info_ea + 2, 2); /* connected */
    vm_write8(info_ea + 6, CELL_AUDIO_OUT_CODING_TYPE_LPCM);
    vm_write8(info_ea + 7, CELL_AUDIO_OUT_CHNUM_2);
    vm_write8(info_ea + 8, CELL_AUDIO_OUT_FS_48KHZ);

    return CELL_OK;
}

s32 cellAudioOutGetConfiguration(u32 audioOut,
                                  CellAudioOutConfiguration* config,
                                  void* option, u32 optionSize)
{
    (void)audioOut;
    (void)option;
    (void)optionSize;

    printf("[cellAvconfExt] AudioOutGetConfiguration()\n");

    if (!config)
        return CELL_EINVAL;

    uint32_t config_ea = (uint32_t)(uintptr_t)config;
    for (uint32_t i = 0; i < sizeof(CellAudioOutConfiguration); i++)
        vm_write8(config_ea + i, 0);
    vm_write8(config_ea + 0, CELL_AUDIO_OUT_CHNUM_2);
    vm_write8(config_ea + 1, CELL_AUDIO_OUT_CODING_TYPE_LPCM);

    return CELL_OK;
}

s32 cellAudioOutSetCopyControl(u32 audioOut, u32 control)
{
    (void)audioOut;
    printf("[cellAvconfExt] AudioOutSetCopyControl(control=%u)\n", control);
    return CELL_OK;
}

s32 cellAudioOutGetNumberOfDevice(u32 audioOut)
{
    (void)audioOut;
    return 1; /* one audio output device */
}

s32 cellVideoOutGetGamma(u32 videoOut, float* gamma)
{
    (void)videoOut;
    if (!gamma) return CELL_EINVAL;
    uint32_t bits;
    memcpy(&bits, &s_gamma, sizeof(bits));
    vm_write32((uint32_t)(uintptr_t)gamma, bits);
    return CELL_OK;
}

s32 cellVideoOutSetGamma(u32 videoOut, float gamma)
{
    (void)videoOut;
    printf("[cellAvconfExt] VideoOutSetGamma(%.2f)\n", gamma);
    s_gamma = gamma;
    return CELL_OK;
}

/* cellAudioOutConfigure(audioOut, config, option, waitForEvent) -- set the
 * audio output mode. We accept any config and report success (request id 0);
 * the title queries availability separately. NID 0x4692AB35. */
s32 cellAudioOutConfigure(u32 audioOut, void* config, void* option, u32 waitForEvent)
{
    (void)config; (void)option; (void)waitForEvent;
    printf("[cellAvconfExt] AudioOutConfigure(audioOut=%u) -> ok\n", audioOut);
    return 0;
}
