/*
 * ps3recomp - cellVdec HLE
 *
 * Video decoder: MPEG-2, H.264/AVC, MPEG-4/DivX decoding through libavcodec.
 * Receives AU (access units) from cellDmux and outputs decoded frames.
 *
 * Constants and guest struct layouts follow the firmware ABI as documented in
 * RPCS3's Emu/Cell/Modules/cellVdec.h. Do not invent values here: a wrong
 * constant is invisible in our own sources and only fails inside a title.
 */

#ifndef PS3RECOMP_CELL_VDEC_H
#define PS3RECOMP_CELL_VDEC_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes
 * -----------------------------------------------------------------------*/
#define CELL_VDEC_ERROR_ARG            0x80610101
#define CELL_VDEC_ERROR_SEQ            0x80610102
#define CELL_VDEC_ERROR_BUSY           0x80610103
#define CELL_VDEC_ERROR_EMPTY          0x80610104
#define CELL_VDEC_ERROR_AU             0x80610105
#define CELL_VDEC_ERROR_PIC            0x80610106
#define CELL_VDEC_ERROR_UNK            0x80610110
#define CELL_VDEC_ERROR_FATAL          0x80610180

/* ---------------------------------------------------------------------------
 * Codec types (CellVdecType::codecType)
 * -----------------------------------------------------------------------*/
#define CELL_VDEC_CODEC_TYPE_MPEG2     0
#define CELL_VDEC_CODEC_TYPE_AVC       1
#define CELL_VDEC_CODEC_TYPE_MPEG4     2
#define CELL_VDEC_CODEC_TYPE_VC1       3
#define CELL_VDEC_CODEC_TYPE_DIVX      5
#define CELL_VDEC_CODEC_TYPE_JVT       7
#define CELL_VDEC_CODEC_TYPE_DIVX3_11  9
#define CELL_VDEC_CODEC_TYPE_MVC       11
#define CELL_VDEC_CODEC_TYPE_MVC2      13

/* ---------------------------------------------------------------------------
 * Picture format (CellVdecPicFormat::formatType)
 * -----------------------------------------------------------------------*/
#define CELL_VDEC_PICFMT_ARGB32_ILV    0
#define CELL_VDEC_PICFMT_RGBA32_ILV    1
#define CELL_VDEC_PICFMT_UYVY422_ILV   2
#define CELL_VDEC_PICFMT_YUV420_PLANAR 3

/* Color matrix (CellVdecPicFormat::colorMatrixType) */
#define CELL_VDEC_COLOR_MATRIX_BT601   0
#define CELL_VDEC_COLOR_MATRIX_BT709   1

/* Picture attribute (CellVdecPicItem::attr) */
#define CELL_VDEC_PICITEM_ATTR_NORMAL  0
#define CELL_VDEC_PICITEM_ATTR_SKIPPED 1

/* Timestamp invalid marker (CellCodecTimeStamp) */
#define CELL_CODEC_PTS_INVALID         0xFFFFFFFFu
#define CELL_CODEC_DTS_INVALID         0xFFFFFFFFu

/* ---------------------------------------------------------------------------
 * Decode mode (cellVdecDecodeAu mode)
 * -----------------------------------------------------------------------*/
#define CELL_VDEC_DEC_MODE_NORMAL      0
#define CELL_VDEC_DEC_MODE_B_SKIP      1
#define CELL_VDEC_DEC_MODE_PB_SKIP     2

/* Universal frame rate codes (cellVdecSetFrameRate) */
#define CELL_VDEC_FRC_24000DIV1001     0x80
#define CELL_VDEC_FRC_24               0x81
#define CELL_VDEC_FRC_25               0x82
#define CELL_VDEC_FRC_30000DIV1001     0x83
#define CELL_VDEC_FRC_30               0x84
#define CELL_VDEC_FRC_50               0x85
#define CELL_VDEC_FRC_60000DIV1001     0x86
#define CELL_VDEC_FRC_60               0x87

/* Callback message types */
#define CELL_VDEC_MSG_TYPE_AUDONE      0
#define CELL_VDEC_MSG_TYPE_PICOUT      1
#define CELL_VDEC_MSG_TYPE_SEQDONE     2
#define CELL_VDEC_MSG_TYPE_ERROR       3

/* ---------------------------------------------------------------------------
 * Types
 *
 * These are HOST mirrors of guest structs. Every field below is a 4-byte
 * scalar except the u64s, and each u64 sits at a naturally aligned offset, so
 * the host layout matches the guest's big-endian layout field for field.
 * Pointer fields live at the END of the guest struct and are read or written
 * by explicit guest offset (see cellVdec.c), never through these mirrors.
 * -----------------------------------------------------------------------*/
typedef u32 CellVdecHandle;

typedef struct CellVdecType {
    u32 codecType;
    u32 profileLevel;
} CellVdecType;

typedef struct CellVdecResource {
    u32 memAddr;
    u32 memSize;
    u32 ppuThreadPriority;
    u32 ppuThreadStackSize;
    u32 spuThreadPriority;
    u32 numOfSpus;
} CellVdecResource;

/* Guest layout: +0 startAddr, +4 size, +8 pts, +16 dts, +24 userData,
 * +32 codecSpecificData. pts/dts are CellCodecTimeStamp (u64 big-endian);
 * 0xFFFFFFFF.. means invalid. */
typedef struct CellVdecAuInfo {
    u32 startAddr;
    u32 size;
    u64 pts;
    u64 dts;
    u64 userData;
    u64 codecSpecificData;
} CellVdecAuInfo;

/* Guest layout: +0 u32 formatType, +4 u32 colorMatrixType, +8 u8 alpha. */
typedef struct CellVdecPicFormat {
    u32 formatType;
    u32 colorMatrixType;
    u8  alpha;
} CellVdecPicFormat;

/* Incomplete on purpose: pic items live only in guest memory (big-endian,
 * written by explicit offset in cellVdec.c); the guest reads them through
 * the guest pointer GetPicItem returns. The name exists so the prototypes
 * below type-check. */
typedef struct CellVdecPicItem CellVdecPicItem;

typedef u32 (*CellVdecCbMsg)(CellVdecHandle handle, u32 msgType,
                               s32 msgData, void* cbArg);

/* Guest callback descriptor: cbFunc is a guest OPD address. */
typedef struct CellVdecCb {
    u32 cbFunc;
    u32 cbArg;
} CellVdecCb;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/
s32 cellVdecQueryAttr(const CellVdecType* type, void* attr);

s32 cellVdecOpen(const CellVdecType* type, const CellVdecResource* res,
                  const CellVdecCb* cb, CellVdecHandle* handle);
s32 cellVdecClose(CellVdecHandle handle);

s32 cellVdecStartSeq(CellVdecHandle handle);
s32 cellVdecEndSeq(CellVdecHandle handle);

s32 cellVdecDecodeAu(CellVdecHandle handle, s32 mode, const CellVdecAuInfo* auInfo);

s32 cellVdecGetPicture(CellVdecHandle handle, const void* format, void* outBuff);
s32 cellVdecGetPicItem(CellVdecHandle handle, const CellVdecPicItem** picItem);

s32 cellVdecSetFrameRate(CellVdecHandle handle, u32 frameRateCode);

/* CellVdecPicItem exists only in guest memory (big-endian, written by
 * explicit offset in cellVdec.c), so no host mirror is declared: the guest
 * reads it through a returned guest pointer. */

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_VDEC_H */
