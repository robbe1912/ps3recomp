/*
 * ps3recomp - cellAvconfExt HLE implementation
 *
 * Reports one HDMI primary audio output with LPCM 2 ch / 5.1 ch / 7.1 ch at
 * 48 kHz. Games query this to decide the audio format before opening
 * cellAudio ports.
 *
 * All output structs are written field-by-field with the vm_write8/16/32
 * accessors (address-translated, big-endian). A host struct store through
 * GUEST_PTR writes u16/u32 fields little-endian into guest RAM, which is how
 * this module once reported "frequency 0" and MPEG2 coding types for LPCM
 * modes (resume_asset_native.log lines 94-140).
 */

#include "cellAvconfExt.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write8/16/32: translate + swap */

#include <stdio.h>

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

    /* Report the SAME capability set a real PS3/RPCS3 reports, not just stereo.
     * Golden trace (RPCS3.log, real YDKJ boot) shows:
     *   cellAudioOut: found support for Linear PCM 2 Ch. / 5.1 Ch. / 7.1 Ch.
     *   cellAudioOut: found support for Dolby Digital 5.1 Ch. / DTS 5.1 Ch.
     * This call returns the MAX channel count available for (type, fs). Under-reporting
     * stereo (the old behaviour) makes FMOD's init probe fail its 8/6-channel checks and
     * then fall through to DTS/AC3 probes that also returned 0 -- YDKJ ends up with no
     * FMOD threads at all. Returning the real 7.1 for LPCM matches the console. */
    if (type == CELL_AUDIO_OUT_CODING_TYPE_LPCM) {
        if (fs & CELL_AUDIO_OUT_FS_48KHZ) return CELL_AUDIO_OUT_CHNUM_6;  /* 5.1 (A/B: was 8) */
        return CELL_AUDIO_OUT_CHNUM_2;
    }

    /* Dolby Digital / DTS: real hardware reports 5.1 for these. */
    if (type == CELL_AUDIO_OUT_CODING_TYPE_AC3 || type == CELL_AUDIO_OUT_CODING_TYPE_DTS)
        return CELL_AUDIO_OUT_CHNUM_6;

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

/* cellAudioOutGetDeviceInfo(audioOut, deviceIndex, info)
 *
 * Guest CellAudioOutDeviceInfo (136 bytes):
 *   +0 u8 portType, +1 u8 availableModeCount, +2 u8 state,
 *   +3 u8 reserved[3], +6 u16 latency, +8 SoundMode[16] (8 bytes each:
 *   u8 type, u8 channel, u8 fs, u8 reserved, u32 layout).
 * The old code wrote a 70-byte host struct through GUEST_PTR, so the title
 * read its modes 8-byte-strided from our 4-byte-strided bytes: every mode
 * came back "MPEG2" (our fs byte 0x04 == CELL_AUDIO_OUT_CODING_TYPE_MPEG2)
 * with an empty frequency list. */
s32 cellAudioOutGetDeviceInfo(u32 audioOut, u32 deviceIndex,
                               CellAudioOutDeviceInfo* info)
{
    (void)audioOut;

    printf("[cellAvconfExt] AudioOutGetDeviceInfo(out=%u, dev=%u)\n",
           audioOut, deviceIndex);

    if (!info)
        return CELL_EINVAL;

    uint32_t ea = (uint32_t)(uintptr_t)info;
    if (deviceIndex >= 1u)   /* cellAudioOutGetNumberOfDevice reports 1 */
        return CELL_AUDIO_OUT_ERROR_PARAMETER_OUT_OF_RANGE;

    /* Zero the whole 136-byte firmware struct (34 words). */
    for (uint32_t a = 0; a < (uint32_t)sizeof(CellAudioOutDeviceInfo); a += 4)
        vm_write32(ea + a, 0);

    /* Report the mode set a real PS3 reports. Golden trace (RPCS3.log, real YDKJ boot):
     *   "found support for Linear PCM 2 Ch. / 5.1 Ch. / 7.1 Ch."
     *   "found support for Dolby Digital 5.1 Ch. / DTS 5.1 Ch."
     * Advertising a single stereo mode here while GetSoundAvailability reports 5.1/7.1 is
     * self-inconsistent, and middleware (FMOD) validates these against each other. */
    static const struct { u8 type, channel, fs; u32 layout; } MODES[3] = {
        { CELL_AUDIO_OUT_CODING_TYPE_LPCM, CELL_AUDIO_OUT_CHNUM_2,
          CELL_AUDIO_OUT_FS_48KHZ, CELL_AUDIO_OUT_SPEAKER_LAYOUT_2CH },
        { CELL_AUDIO_OUT_CODING_TYPE_LPCM, CELL_AUDIO_OUT_CHNUM_6,
          CELL_AUDIO_OUT_FS_48KHZ, CELL_AUDIO_OUT_SPEAKER_LAYOUT_6CH_LREClr },
        { CELL_AUDIO_OUT_CODING_TYPE_LPCM, CELL_AUDIO_OUT_CHNUM_8,
          CELL_AUDIO_OUT_FS_48KHZ, CELL_AUDIO_OUT_SPEAKER_LAYOUT_8CH_LREClrxy },
    };

    vm_write8(ea + 0, CELL_AUDIO_OUT_PORT_HDMI);
    vm_write8(ea + 1, 3);                                    /* mode count  */
    vm_write8(ea + 2, CELL_AUDIO_OUT_DEVICE_STATE_AVAILABLE);
    vm_write16(ea + 6, 13);                                  /* HDMI latency */
    for (u32 i = 0; i < 3; i++) {
        uint32_t m = ea + 8 + i * (uint32_t)sizeof(CellAudioOutSoundMode);
        vm_write8(m + 0, MODES[i].type);
        vm_write8(m + 1, MODES[i].channel);
        vm_write8(m + 2, MODES[i].fs);
        vm_write32(m + 4, MODES[i].layout);
    }

    return CELL_OK;
}

/* cellAudioOutGetConfiguration(audioOut, config, option, optionSize)
 *
 * Guest CellAudioOutConfiguration (16 bytes): u8 channel, u8 encoder,
 * u8 reserved[10], u32 downMixer. */
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

    uint32_t ea = (uint32_t)(uintptr_t)config;
    for (uint32_t a = 0; a < (uint32_t)sizeof(CellAudioOutConfiguration); a += 4)
        vm_write32(ea + a, 0);
    vm_write8(ea + 0, CELL_AUDIO_OUT_CHNUM_2);              /* channel     */
    vm_write8(ea + 1, CELL_AUDIO_OUT_CODING_TYPE_LPCM);     /* encoder     */
    vm_write32(ea + 12, CELL_AUDIO_OUT_DOWNMIXER_NONE);

    return CELL_OK;
}

/* cellAudioOutGetState(audioOut, deviceIndex, state) -- the audio sibling of
 * cellVideoOutGetState. A title checks it before configuring output; Virtua
 * Fighter 5 calls it once during boot and it was the last unresolved import in
 * the whole title. The device is always present and enabled here: there is no
 * "no audio device" case a recompiled port can be in.
 *
 * Guest CellAudioOutState (20 bytes): u8 state, u8 encoder, u8 reserved[6],
 * u32 downMixer, CellAudioOutSoundMode soundMode at +12.
 * The old code wrote channel/fs at +9/+10 -- those are reserved bytes; the
 * title read channels and frequency from +13/+14, both zero
 * ("Audio: Current Frequency 0.000000Khz ... Current Channels 0"). */
s32 cellAudioOutGetState(u32 audioOut, u32 deviceIndex, CellAudioOutState* state)
{
    (void)deviceIndex;

    uint32_t ea = (uint32_t)(uintptr_t)state;
    printf("[cellAvconfExt] AudioOutGetState(audioOut=%u)\n", audioOut);
    if (!ea)
        return CELL_EINVAL;

    /* Zero the whole 20-byte firmware struct, then fill the fields. The
     * current configuration is stereo LPCM 48 kHz (see GetConfiguration). */
    for (uint32_t a = 0; a < (uint32_t)sizeof(CellAudioOutState); a += 4)
        vm_write32(ea + a, 0);
    vm_write8(ea + 0, CELL_AUDIO_OUT_OUTPUT_STATE_ENABLED);
    vm_write8(ea + 1, CELL_AUDIO_OUT_CODING_TYPE_LPCM);
    vm_write32(ea + 8, CELL_AUDIO_OUT_DOWNMIXER_NONE);
    vm_write8(ea + 12, CELL_AUDIO_OUT_CODING_TYPE_LPCM);    /* soundMode   */
    vm_write8(ea + 13, CELL_AUDIO_OUT_CHNUM_2);
    vm_write8(ea + 14, CELL_AUDIO_OUT_FS_48KHZ);
    vm_write32(ea + 16, CELL_AUDIO_OUT_SPEAKER_LAYOUT_2CH);
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
    { float g = s_gamma; uint32_t bits; memcpy(&bits, &g, 4);
      vm_write32((uint32_t)(uintptr_t)gamma, bits); }
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
