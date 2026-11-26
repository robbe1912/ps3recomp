/*
 * ps3recomp - cellAvconfExt HLE
 *
 * Extended audio/video configuration. Queries display capabilities,
 * HDMI info, and audio output parameters.
 *
 * Enum values and struct layouts below are the firmware ABI as documented in
 * RPCS3's Emu/Cell/Modules/cellAudioOut.h. A title reads these structures
 * from big-endian guest RAM, so every field wider than u8 must be written
 * with the vm_write16/vm_write32 accessors -- never with a host struct store
 * through GUEST_PTR, which would land byte-reversed in guest memory (this
 * file shipped that bug once: the title read "frequency 0", MPEG2 coding
 * types and empty frequency lists from LPCM 2/5.1/7.1 modes).
 */

#ifndef PS3RECOMP_CELL_AVCONF_EXT_H
#define PS3RECOMP_CELL_AVCONF_EXT_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Audio output selectors
 * -----------------------------------------------------------------------*/
#define CELL_AUDIO_OUT_PRIMARY             0
#define CELL_AUDIO_OUT_SECONDARY           1

/* ---------------------------------------------------------------------------
 * Error codes (CellAudioOutError)
 * -----------------------------------------------------------------------*/
#define CELL_AUDIO_OUT_ERROR_ILLEGAL_PARAMETER      0x8002B242u
#define CELL_AUDIO_OUT_ERROR_PARAMETER_OUT_OF_RANGE 0x8002B243u

/* ---------------------------------------------------------------------------
 * Port types (CellAudioOutPortType)
 * -----------------------------------------------------------------------*/
#define CELL_AUDIO_OUT_PORT_HDMI           0
#define CELL_AUDIO_OUT_PORT_SPDIF          1
#define CELL_AUDIO_OUT_PORT_ANALOG         2
#define CELL_AUDIO_OUT_PORT_USB            3
#define CELL_AUDIO_OUT_PORT_BLUETOOTH      4
#define CELL_AUDIO_OUT_PORT_NETWORK        5

/* Device states (CellAudioOutDeviceState) */
#define CELL_AUDIO_OUT_DEVICE_STATE_UNAVAILABLE 0
#define CELL_AUDIO_OUT_DEVICE_STATE_AVAILABLE   1

/* Output states (CellAudioOutOutputState) */
#define CELL_AUDIO_OUT_OUTPUT_STATE_ENABLED   0
#define CELL_AUDIO_OUT_OUTPUT_STATE_DISABLED  1
#define CELL_AUDIO_OUT_OUTPUT_STATE_PREPARING 2

/* ---------------------------------------------------------------------------
 * Coding types (CellAudioOutCodingType)
 * -----------------------------------------------------------------------*/
#define CELL_AUDIO_OUT_CODING_TYPE_LPCM      0
#define CELL_AUDIO_OUT_CODING_TYPE_AC3       1
#define CELL_AUDIO_OUT_CODING_TYPE_MPEG1     2
#define CELL_AUDIO_OUT_CODING_TYPE_MP3       3
#define CELL_AUDIO_OUT_CODING_TYPE_MPEG2     4
#define CELL_AUDIO_OUT_CODING_TYPE_AAC       5
#define CELL_AUDIO_OUT_CODING_TYPE_DTS       6
#define CELL_AUDIO_OUT_CODING_TYPE_ATRAC     7
#define CELL_AUDIO_OUT_CODING_TYPE_BITSTREAM 0xFF

/* Channel counts (CellAudioOutChnum) */
#define CELL_AUDIO_OUT_CHNUM_2             2
#define CELL_AUDIO_OUT_CHNUM_4             4
#define CELL_AUDIO_OUT_CHNUM_6             6
#define CELL_AUDIO_OUT_CHNUM_8             8

/* Sampling rates (CellAudioOutFs): bit mask, one bit per rate */
#define CELL_AUDIO_OUT_FS_32KHZ            0x01
#define CELL_AUDIO_OUT_FS_44KHZ            0x02
#define CELL_AUDIO_OUT_FS_48KHZ            0x04
#define CELL_AUDIO_OUT_FS_88KHZ            0x08
#define CELL_AUDIO_OUT_FS_96KHZ            0x10
#define CELL_AUDIO_OUT_FS_176KHZ           0x20
#define CELL_AUDIO_OUT_FS_192KHZ           0x40

/* Speaker layouts (CellAudioOutSpeakerLayout) */
#define CELL_AUDIO_OUT_SPEAKER_LAYOUT_DEFAULT      0x00000000u
#define CELL_AUDIO_OUT_SPEAKER_LAYOUT_2CH          0x00000001u
#define CELL_AUDIO_OUT_SPEAKER_LAYOUT_6CH_LREClr   0x00010000u
#define CELL_AUDIO_OUT_SPEAKER_LAYOUT_8CH_LREClrxy 0x40000000u

/* Down-mixers (CellAudioOutDownMixer) */
#define CELL_AUDIO_OUT_DOWNMIXER_NONE     0
#define CELL_AUDIO_OUT_DOWNMIXER_TYPE_A   1
#define CELL_AUDIO_OUT_DOWNMIXER_TYPE_B   2

/* ---------------------------------------------------------------------------
 * Types
 *
 * HOST mirrors of the firmware structs. Every field wider than u8 sits at a
 * naturally aligned offset, so host layout matches guest layout and the
 * static asserts below pin that. Guest RAM is big-endian: these mirrors are
 * documentation and size anchors ONLY -- cellAvconfExt.c writes each field
 * through vm_write8/16/32 at the documented offsets, never a struct store.
 * -----------------------------------------------------------------------*/

/* Guest: +0 u8 type, +1 u8 channel, +2 u8 fs, +3 u8 reserved,
 *        +4 be32 layout. Size 8. */
typedef struct CellAudioOutSoundMode {
    u8  type;
    u8  channel;
    u8  fs;
    u8  reserved;
    u32 layout;
} CellAudioOutSoundMode;

/* Guest: +0 u8 portType, +1 u8 availableModeCount, +2 u8 state,
 *        +3 u8 reserved[3], +6 be16 latency,
 *        +8 CellAudioOutSoundMode[16]. Size 136. */
typedef struct CellAudioOutDeviceInfo {
    u8  portType;
    u8  availableModeCount;
    u8  state;
    u8  reserved[3];
    u16 latency;
    CellAudioOutSoundMode availableModes[16];
} CellAudioOutDeviceInfo;

/* Guest: +0 u8 channel, +1 u8 encoder, +2 u8 reserved[10],
 *        +12 be32 downMixer. Size 16. */
typedef struct CellAudioOutConfiguration {
    u8  channel;
    u8  encoder;
    u8  reserved[10];
    u32 downMixer;
} CellAudioOutConfiguration;

/* Guest: +0 u8 state, +1 u8 encoder, +2 u8 reserved[6],
 *        +8 be32 downMixer, +12 CellAudioOutSoundMode soundMode. Size 20. */
typedef struct CellAudioOutState {
    u8  state;
    u8  encoder;
    u8  reserved[6];
    u32 downMixer;
    CellAudioOutSoundMode soundMode;
} CellAudioOutState;

_Static_assert(sizeof(CellAudioOutSoundMode) == 8, "firmware ABI");
_Static_assert(offsetof(CellAudioOutSoundMode, layout) == 4, "firmware ABI");

_Static_assert(sizeof(CellAudioOutDeviceInfo) == 136, "firmware ABI");
_Static_assert(offsetof(CellAudioOutDeviceInfo, latency) == 6, "firmware ABI");
_Static_assert(offsetof(CellAudioOutDeviceInfo, availableModes) == 8, "firmware ABI");

_Static_assert(sizeof(CellAudioOutConfiguration) == 16, "firmware ABI");
_Static_assert(offsetof(CellAudioOutConfiguration, downMixer) == 12, "firmware ABI");

_Static_assert(sizeof(CellAudioOutState) == 20, "firmware ABI");
_Static_assert(offsetof(CellAudioOutState, downMixer) == 8, "firmware ABI");
_Static_assert(offsetof(CellAudioOutState, soundMode) == 12, "firmware ABI");

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 cellAudioOutGetSoundAvailability(u32 audioOut, u32 type, u32 fs, u32 option);
s32 cellAudioOutGetSoundAvailability2(u32 audioOut, u32 type, u32 fs, u32 ch, u32 option);

s32 cellAudioOutGetDeviceInfo(u32 audioOut, u32 deviceIndex,
                               CellAudioOutDeviceInfo* info);
s32 cellAudioOutGetConfiguration(u32 audioOut,
                                  CellAudioOutConfiguration* config,
                                  void* option, u32 optionSize);
s32 cellAudioOutGetState(u32 audioOut, u32 deviceIndex, CellAudioOutState* state);
s32 cellAudioOutSetCopyControl(u32 audioOut, u32 control);

/* HDMI/display audio */
s32 cellAudioOutGetNumberOfDevice(u32 audioOut);

/* Video output extended queries */
s32 cellVideoOutGetGamma(u32 videoOut, float* gamma);
s32 cellVideoOutSetGamma(u32 videoOut, float gamma);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_AVCONF_EXT_H */
