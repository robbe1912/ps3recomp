/*
 * ps3recomp - cellVdec HLE implementation
 *
 * Real video decoding through libavcodec + libswscale (LGPL shared build).
 * Each open decoder owns one host worker thread. DecodeAu copies the AU bytes
 * out of guest memory into a padded owned buffer and queues a job, answering
 * BUSY (retryable) instead of blocking when its queue is full; the worker
 * pushes the bytes through an AVCodecParser (so AU boundaries from cellDmux
 * do not have to be picture boundaries) into the decoder, fires AUDONE once
 * the AU is consumed, and then queues one AVFrame reference per decoded
 * picture, firing PICOUT for each -- AUDONE always precedes that AU's
 * PICOUTs, like the firmware. GetPicItem describes each queued picture once
 * (afterwards EMPTY until the next picture) into a slot of the title-provided
 * work memory; GetPicture converts it with point filtering into the requested
 * ARGB/RGBA/UYVY/YUV420 layout and consumes it. EndSeq drains parser and
 * decoder before SEQDONE; StartSeq replaces the sequence atomically and never
 * blocks the caller; DecodeAu/GetPicture/GetPicItem never block either.
 * Close stops the worker, frees everything and wakes waiters.
 *
 * Guest callbacks are fired from the worker thread through g_ps3_guest_caller,
 * never while a decoder lock is held.
 */

#include "cellVdec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../guest_struct.h"   /* GUEST_EA, vm_read/vm_write: guest EA -> host */
#include "ps3emu/guest_call.h" /* g_ps3_guest_caller -- cbFunc is a GUEST OPD */
#include "../../runtime/memory/vm.h"     /* VM_HLE_INJECT_BASE */

#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>  /* av_image_get_buffer_size (item size) */
#include <libswscale/swscale.h>

/* ---------------------------------------------------------------------------
 * Platform threading, after cellAudio.c's conventions
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
  #include <windows.h>
  #include <process.h>
  typedef HANDLE thread_handle_t;
  typedef CRITICAL_SECTION mutex_t;
  typedef CONDITION_VARIABLE cond_t;
  #define mutex_init(m)     InitializeCriticalSection(m)
  #define mutex_destroy(m) DeleteCriticalSection(m)
  #define mutex_lock(m)     EnterCriticalSection(m)
  #define mutex_unlock(m)   LeaveCriticalSection(m)
  #define cond_init(c)      InitializeConditionVariable(c)
  #define cond_destroy(c)   (void)(c)
  #define cond_wait(c, m)   SleepConditionVariableCS((c), (m), INFINITE)
  #define cond_broadcast(c) WakeAllConditionVariable(c)
#else
  #include <pthread.h>
  #include <unistd.h>
  typedef pthread_t thread_handle_t;
  typedef pthread_mutex_t mutex_t;
  typedef pthread_cond_t cond_t;
  #define mutex_init(m)     pthread_mutex_init((m), NULL)
  #define mutex_destroy(m)  pthread_mutex_destroy(m)
  #define mutex_lock(m)     pthread_mutex_lock(m)
  #define mutex_unlock(m)   pthread_mutex_unlock(m)
  #define cond_init(c)      pthread_cond_init((c), NULL)
  #define cond_destroy(c)   pthread_cond_destroy(c)
  #define cond_wait(c, m)   pthread_cond_wait((c), (m))
  #define cond_broadcast(c) pthread_cond_broadcast(c)
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/
#define MAX_VDEC 4

/* DecodeAu returns BUSY when the AU queue is full. Only the worker waits for
 * picture space; StartSeq and Close interrupt that wait. */
#define VDEC_AU_QUEUE   16
#define VDEC_PIC_QUEUE  8
/* Frames the decoder emits for one AU before the queue drains them. Picture
 * AUs yield one; pathological multi-picture AUs are capped here. */
#define VDEC_COLLECT_MAX 16

/* One submitted access unit. `data` is an owned, AV_INPUT_BUFFER_PADDING_SIZE
 * padded copy: once DecodeAu returns, nothing points into guest memory. */
typedef struct {
    u8*   data;
    u32   size;
    u64   pts, dts, userData;
    int   pts_valid, dts_valid;
    u32   epoch;             /* seq_epoch at submission: stale AUs are dropped */
    int   mode;              /* CellVdecDecodeMode -> item attr NORMAL/SKIPPED */
} VdecAuJob;

/* One decoded picture. The AVFrame reference stays queued until GetPicture
 * consumes it; GetPicItem only peeks, once per picture (firmware hands a
 * picture's item out exactly once; a second peek reports EMPTY). sar/framerate
 * are sampled from avctx on the worker thread, because the caller thread must
 * not read avctx while the worker keeps decoding into it. */
typedef struct {
    AVFrame* frame;          /* owned allocation, ref'd per picture */
    u64   pts, dts, userData;  /* pts/dts chains: always valid, synthesized */
    int   sar_num, sar_den;
    int   fr_num, fr_den;
    u32   frc;               /* CellVdecFrameRate code for the item */
    u32   attr;              /* CELL_VDEC_PICITEM_ATTR_* from the AU's mode */
    int   item_taken;        /* GetPicItem already returned this picture */
} VdecPic;

/* Copied into packet opaque_ref, which libavcodec carries through reordering. */
typedef struct {
    u64 userData;
    s32 mode;
    int64_t dts;
    u32 epoch;
} VdecAuMeta;

/* Parser input spans stay alive until it advances past them. A picture can
 * start in an earlier AU and finish when a later AU supplies its delimiter. */
typedef struct VdecAuSpan {
    u64 start, end;
    int64_t pts, dts;
    VdecAuMeta meta;
    struct VdecAuSpan* next;
} VdecAuSpan;

typedef struct {
    int in_use;
    int closing;             /* set by Close; wakes every waiter */
    int fatal;               /* decoder unusable: bring-up or alloc failed */
    u32 codecType;           /* CellVdecCodecType as the guest passed it */
    enum AVCodecID ff_id;
    u32 cbFunc;              /* guest EA of the callback's OPD (set once) */
    u32 cbArg;               /* guest EA handed back to it (set once) */
    u32   seq_epoch;         /* bumped by StartSeq; stale work is dropped */
    u8    seq_state;         /* 0 dormant, 1 ready, 2 ending (like firmware) */
    u64   next_pts, next_dts;/* presentation/dispatch stamp chains */
    u32   frameRateCode;     /* last SetFrameRate value (0x80..0x87) */
    u32   frc_set;           /* frame-rate override from SetFrameRate */
    u32   item_base;         /* title-provided item memory (res.memAddr) */
    u32   item_slots;        /* 0x200-byte item slots available there */
    u32   item_bias;         /* round-robin slot for the next GetPicItem */

    mutex_t lock;            /* guards everything below on the API side */
    cond_t  cv;
    thread_handle_t thread;
    int thread_live;

    VdecAuJob jobs[VDEC_AU_QUEUE];
    u32 job_head, job_count;

    VdecPic pics[VDEC_PIC_QUEUE];
    u32 pic_head, pic_count;

    int flush_pending;       /* EndSeq: drain, then SEQDONE */
    int reset_pending;       /* StartSeq: worker-side codec reset */

    /* Worker-thread-owned (no lock): the parser and decoder state. */
    AVCodecContext*       avctx;
    AVCodecParserContext* parser;
    AVPacket*             pkt;
    AVFrame*              dec_frame;
    u8*  carry;             /* parser input carry across AUs */
    u32 carry_len, carry_cap;
    u64 input_offset, carry_offset;
    VdecAuSpan* spans;
    VdecAuSpan* span_tail;
    AVFrame* collect[VDEC_COLLECT_MAX];
    u32 collect_n;

    /* Caller-thread-owned (GetPicture path). */
    struct SwsContext* sws;
    AVFrame* getpic_frame;   /* popped picture while it is being converted */
} VdecSlot;

static VdecSlot s_vdec[MAX_VDEC];

/* Guest-resident pic items, in the HLE inject window after cellAdec's scratch
 * (VM_HLE_INJECT_BASE + 0x40000..0x50000). One 0x200-byte block per decoder:
 * the CellVdecPicItem at +0, the codec info block it points to at +0x80 (the
 * CellVdecMpeg2Info is 0x160 bytes, which the old 0x100 stride truncated). */
#define VDEC_ITEM_EA(h)  (VM_HLE_INJECT_BASE + 0x50000u + (u32)(h) * 0x200u)
#define VDEC_INFO_EA(h)  (VDEC_ITEM_EA(h) + 0x80u)

/* VDEC_TRACE=1: bounded tracing of the decode path. The native bridge has no
 * other way to show which vdec entry points a title's player reaches; every
 * site prints its first 64 hits, then every 512th, so a log stays readable. */
static int vdec_trace_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("VDEC_TRACE");
        on = (e && *e && e[0] != '0') ? 1 : 0;
    }
    return on;
}
#define VDEC_TRACE(...)                                                    \
    do {                                                                   \
        static unsigned vdec_trace_n_;                                     \
        if (vdec_trace_enabled()                                           \
            && (vdec_trace_n_++ < 64 || vdec_trace_n_ % 512 == 0)) {       \
            fprintf(stderr, "[cellVdec] ");                               \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

/* Deliver a vdec message to the guest callback (handle, msgType, msgData,
 * cbArg). The guest's cbFunc is an OPD, so this goes through the recompiled
 * code's dispatcher; called from the worker thread. Callers must not hold the
 * slot lock: a guest callback that re-enters cellVdec would self-deadlock. */
static void vdec_notify(const VdecSlot* v, CellVdecHandle handle,
                        u32 msg_type, s32 msg_data)
{
    VDEC_TRACE("cb handle=%u type=%u data=%d opd=0x%08X",
               handle, msg_type, msg_data, v->cbFunc);
    if (!v->cbFunc || !g_ps3_guest_caller) return;
    g_ps3_guest_caller(v->cbFunc, (u64)handle, (u64)msg_type,
                       (u64)(s64)msg_data, (u64)v->cbArg, 0, 0, 0, 0);
}

/* The parser reports each packet's first byte offset. Attribute the packet
 * to that input span, not to the AU that happened to finish the picture.
 * Packet references own their metadata independently of these spans. */
static const VdecAuMeta* vdec_packet_meta(VdecSlot* v, u64 offset)
{
    while (v->spans && v->spans->end <= offset) {
        VdecAuSpan* old = v->spans;
        v->spans = old->next;
        free(old);
    }
    if (!v->spans) v->span_tail = NULL;
    if (v->spans && v->spans->start <= offset)
        return &v->spans->meta;
    return NULL;
}

static void vdec_clear_spans(VdecSlot* v)
{
    while (v->spans) {
        VdecAuSpan* old = v->spans;
        v->spans = old->next;
        free(old);
    }
    v->span_tail = NULL;
    v->input_offset = v->carry_offset = 0;
}

/* ---------------------------------------------------------------------------
 * Worker thread
 * -----------------------------------------------------------------------*/

/* Map an (num:den) frame rate to its CellVdecFrameRate code; 0 when unknown.
 * The title may override both this and the stream rate via SetFrameRate. */
static u32 vdec_frc_from_rate(int num, int den)
{
    if (den <= 0) return 0;
    struct { int num, den; u32 code; } tab[] = {
        { 24000, 1001, 0x80 }, { 24, 1, 0x81 }, { 25, 1, 0x82 },
        { 30000, 1001, 0x83 }, { 30, 1, 0x84 }, { 50, 1, 0x85 },
        { 60000, 1001, 0x86 }, { 60, 1, 0x87 },
    };
    for (unsigned i = 0; i < sizeof(tab)/sizeof(tab[0]); i++)
        if (tab[i].num == num && tab[i].den == den) return tab[i].code;
    return 0;
}

/* One frame duration in 90 kHz ticks: the SetFrameRate override when set,
 * else the stream frame rate, else the firmware's 30 fps fallback. */
static u64 vdec_frame_amend(const VdecSlot* v, u32 frc_set,
                            int fr_num, int fr_den)
{
    switch (frc_set) {
    case 0x80: return (u64)1001 * 90000 / 24000;
    case 0x81: return 90000 / 24;
    case 0x82: return 90000 / 25;
    case 0x83: return (u64)1001 * 90000 / 30000;
    case 0x84: return 90000 / 30;
    case 0x85: return 90000 / 50;
    case 0x86: return (u64)1001 * 90000 / 60000;
    case 0x87: return 90000 / 60;
    }
    if (fr_num > 0 && fr_den > 0)
        return (u64)90000 * (u64)fr_den / (u64)fr_num;
    (void)v;
    return 3000;
}

/* Queue one decoded frame. Waits while the picture queue is full -- but a
 * sequence reset (StartSeq) or close breaks the wait so the worker can never
 * wedge behind a consumer that stopped draining; the frame is dropped then,
 * exactly like the firmware drops stale-sequence pictures. Fires PICOUT after
 * the frame is queued, with no lock held. Returns 0, or -1 when dropped. */
static int vdec_queue_pic(VdecSlot* v, CellVdecHandle handle, AVFrame* frame,
                          u32 epoch)
{
    mutex_lock(&v->lock);
    while (v->pic_count == VDEC_PIC_QUEUE && !v->closing
           && v->seq_epoch == epoch)
        cond_wait(&v->cv, &v->lock);
    if (v->closing || v->seq_epoch != epoch) {
        mutex_unlock(&v->lock);
        return -1;
    }

    const VdecAuMeta* e = frame->opaque_ref
        ? (const VdecAuMeta*)frame->opaque_ref->data : NULL;
    if (!e || e->epoch != epoch) {
        mutex_unlock(&v->lock);
        vdec_notify(v, handle, CELL_VDEC_MSG_TYPE_ERROR,
                    (s32)CELL_VDEC_ERROR_FATAL);
        return -1;
    }

    VdecPic* p = &v->pics[(v->pic_head + v->pic_count) % VDEC_PIC_QUEUE];
    av_frame_unref(p->frame);
    if (av_frame_ref(p->frame, frame) < 0) {
        mutex_unlock(&v->lock);
        return -1;
    }
    /* AU stamps seed the chains. Picture PTS and the source packet's DTS
     * re-anchor them after reordering. Unstamped pictures use the running
     * values, and each picture advances both chains by one frame duration. */
    if (frame->pts != AV_NOPTS_VALUE)     v->next_pts = (u64)frame->pts;
    if (e->dts != AV_NOPTS_VALUE) v->next_dts = (u64)e->dts;
    p->pts = v->next_pts;
    p->dts = v->next_dts;
    p->userData = e->userData;
    p->attr = e->mode == CELL_VDEC_DEC_MODE_NORMAL
        ? (u32)CELL_VDEC_PICITEM_ATTR_NORMAL : (u32)CELL_VDEC_PICITEM_ATTR_SKIPPED;
    p->sar_num = v->avctx->sample_aspect_ratio.num;
    p->sar_den = v->avctx->sample_aspect_ratio.den;
    p->fr_num = v->avctx->framerate.num;
    p->fr_den = v->avctx->framerate.den;
    p->frc = v->frc_set ? v->frc_set
                        : vdec_frc_from_rate(p->fr_num, p->fr_den);
    const u64 amend = vdec_frame_amend(v, v->frc_set, p->fr_num, p->fr_den);
    v->next_pts += amend;
    v->next_dts += amend;
    p->item_taken = 0;
    v->pic_count++;
    mutex_unlock(&v->lock);

    vdec_notify(v, handle, CELL_VDEC_MSG_TYPE_PICOUT, CELL_OK);
    return 0;
}

/* Receive every frame the decoder currently has output into the per-AU
 * collect array (worker-owned). The AU's frames are queued -- and PICOUT
 * fires -- only after AUDONE, which is the firmware's callback order. */
static void vdec_receive_collect(VdecSlot* v)
{
    for (;;) {
        int ret = avcodec_receive_frame(v->avctx, v->dec_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;                      /* corrupt input: drop and go on */
        if (v->collect_n < VDEC_COLLECT_MAX) {
            if (av_frame_ref(v->collect[v->collect_n], v->dec_frame) == 0)
                v->collect_n++;
        }
        av_frame_unref(v->dec_frame);
    }
}

/* Move the collected frames of one AU (or one drain) into the picture queue. */
static void vdec_flush_collected(VdecSlot* v, CellVdecHandle handle,
                                 u32 epoch)
{
    for (u32 i = 0; i < v->collect_n; i++) {
        vdec_queue_pic(v, handle, v->collect[i], epoch);
        av_frame_unref(v->collect[i]);
    }
    v->collect_n = 0;
}

/* Send one parser packet with reference-counted source metadata. The decoder
 * propagates it to the corresponding frame, including delayed B pictures. */
static void vdec_send_packet(VdecSlot* v, CellVdecHandle handle,
                             const u8* data, int size, int64_t pts, int64_t dts)
{
    const VdecAuMeta* meta = vdec_packet_meta(v, (u64)v->parser->frame_offset);
    av_packet_unref(v->pkt);
    v->pkt->data = (u8*)data;
    v->pkt->size = size;
    v->pkt->pts = pts;
    v->pkt->dts = dts;
    if (!meta || !(v->pkt->opaque_ref = av_buffer_alloc(sizeof(*meta)))) {
        vdec_notify(v, handle, CELL_VDEC_MSG_TYPE_ERROR,
                    (s32)CELL_VDEC_ERROR_FATAL);
        av_packet_unref(v->pkt);
        return;
    }
    memcpy(v->pkt->opaque_ref->data, meta, sizeof(*meta));
    ((VdecAuMeta*)v->pkt->opaque_ref->data)->dts = dts;
    /* A delimiter can arrive in an AU with a different decode mode. */
    v->avctx->skip_frame = meta->mode == CELL_VDEC_DEC_MODE_B_SKIP
        ? AVDISCARD_BIDIR : meta->mode == CELL_VDEC_DEC_MODE_PB_SKIP
        ? AVDISCARD_NONKEY : AVDISCARD_DEFAULT;
    int ret = avcodec_send_packet(v->avctx, v->pkt);
    if (ret == AVERROR(EAGAIN)) {
        vdec_receive_collect(v);
        ret = avcodec_send_packet(v->avctx, v->pkt);
    }
    if (ret == 0) vdec_receive_collect(v);
    else vdec_notify(v, handle, CELL_VDEC_MSG_TYPE_ERROR,
                     (s32)CELL_VDEC_ERROR_AU);
    av_packet_unref(v->pkt);
}

/* Decode one submitted AU. The parser is fed from the carry buffer (leftover
 * bytes from earlier AUs) plus this AU, so cellDmux's AU boundaries do not
 * have to line up with picture boundaries. Callback order matches the
 * firmware: AUDONE once every byte of the AU has been handed to the parser,
 * then one PICOUT per picture the AU produced (even when the AU's sequence
 * has meanwhile been replaced -- stale pictures are simply dropped). */
static void vdec_decode_job(VdecSlot* v, CellVdecHandle handle, VdecAuJob* job)
{
    int stale = 0;
    mutex_lock(&v->lock);
    if (job->epoch != v->seq_epoch) stale = 1;
    else {
        /* First valid AU stamps seed the presentation chains (firmware
         * semantics: only when the chain is still at zero). */
        if (v->next_pts == 0 && job->pts_valid) v->next_pts = job->pts;
        if (v->next_dts == 0 && job->dts_valid) v->next_dts = job->dts;
    }
    mutex_unlock(&v->lock);

    if (job->size == 0 || stale) {
        /* An AU of a replaced sequence still completes -- without decoding. */
        vdec_notify(v, handle, CELL_VDEC_MSG_TYPE_AUDONE, CELL_OK);
        return;
    }

    VdecAuSpan* span = (VdecAuSpan*)malloc(sizeof(*span));
    if (!span) {
        vdec_notify(v, handle, CELL_VDEC_MSG_TYPE_ERROR,
                    (s32)CELL_VDEC_ERROR_FATAL);
        vdec_notify(v, handle, CELL_VDEC_MSG_TYPE_AUDONE, CELL_OK);
        return;
    }
    span->start = v->input_offset;
    span->end = span->start + job->size;
    span->meta.userData = job->userData;
    span->meta.mode = job->mode;
    span->meta.dts = AV_NOPTS_VALUE;
    span->meta.epoch = job->epoch;
    span->pts = job->pts_valid ? (int64_t)job->pts : AV_NOPTS_VALUE;
    span->dts = job->dts_valid ? (int64_t)job->dts : AV_NOPTS_VALUE;
    span->next = NULL;

    u32 need = v->carry_len + job->size + AV_INPUT_BUFFER_PADDING_SIZE;
    if (need > v->carry_cap) {
        u8* nb = (u8*)realloc(v->carry, need);
        if (!nb) {
            /* Host out of memory. The AU is accepted, so it must still
             * complete: report the decode error, then fire AUDONE. */
            free(span);
            vdec_notify(v, handle, CELL_VDEC_MSG_TYPE_ERROR,
                        (s32)CELL_VDEC_ERROR_FATAL);
            vdec_notify(v, handle, CELL_VDEC_MSG_TYPE_AUDONE, CELL_OK);
            return;
        }
        v->carry = nb;
        v->carry_cap = need;
    }
    if (v->span_tail) v->span_tail->next = span;
    else v->spans = span;
    v->span_tail = span;
    v->input_offset = span->end;
    memcpy(v->carry + v->carry_len, job->data, job->size);
    v->carry_len += job->size;
    memset(v->carry + v->carry_len, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    u32 off = 0;
    while (off < v->carry_len) {
        const u64 pos = v->carry_offset + off;
        const VdecAuSpan* input = v->spans;
        while (input && input->end <= pos) input = input->next;
        const u32 len = input && input->end - pos < v->carry_len - off
            ? (u32)(input->end - pos) : v->carry_len - off;
        u8* pdata = NULL;
        int psize = 0;
        int ret = av_parser_parse2(v->parser, v->avctx, (u8**)&pdata, &psize,
                                   v->carry + off, (int)len,
                                   input && pos == input->start
                                       ? input->pts : AV_NOPTS_VALUE,
                                   input && pos == input->start
                                       ? input->dts : AV_NOPTS_VALUE,
                                   (int64_t)pos);
        if (ret < 0) {
            vdec_notify(v, handle, CELL_VDEC_MSG_TYPE_ERROR, ret);
            off = v->carry_len;
            break;
        }
        off += (u32)ret;
        if (psize > 0) {
            /* The parser reports the picture's own stamps. MPEG-2 B pictures
             * carry none in the stream, so those stay AV_NOPTS here and the
             * presentation chain fills the picture item later. */
            vdec_send_packet(v, handle, pdata, psize,
                             v->parser->pts, v->parser->dts);
        }
        if (ret == 0 && psize == 0)
            break;                                /* parser wants more input */
    }
    if (off) {
        memmove(v->carry, v->carry + off, v->carry_len - off);
        v->carry_len -= off;
        v->carry_offset += off;
        memset(v->carry + v->carry_len, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    }

    vdec_notify(v, handle, CELL_VDEC_MSG_TYPE_AUDONE, CELL_OK);
    vdec_flush_collected(v, handle, job->epoch);
}

/* EndSeq: flush the parser's pending picture, drain every delayed decoder
 * frame (one PICOUT each), then fire SEQDONE and return the sequence to the
 * dormant state. Called with no lock held. */
static void vdec_flush(VdecSlot* v, CellVdecHandle handle, u32 epoch)
{
    /* Keep the active skip mode. A drain must not revive skipped pictures. */
    for (;;) {
        u8* pdata = NULL;
        int psize = 0;
        int ret = av_parser_parse2(v->parser, v->avctx, (u8**)&pdata, &psize,
                                   NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (psize > 0)
            vdec_send_packet(v, handle, pdata, psize,
                             v->parser->pts, v->parser->dts);
        if (ret <= 0) break;                      /* parser fully flushed */
    }

    avcodec_send_packet(v->avctx, NULL);          /* enter drain mode */
    vdec_receive_collect(v);
    vdec_flush_collected(v, handle, epoch);
    /* The decoder is now at EOF; further AUs need a clean context. */
    avcodec_flush_buffers(v->avctx);
    v->carry_len = 0;

    mutex_lock(&v->lock);
    const int current = v->seq_epoch == epoch && !v->closing;
    if (current && v->seq_state == 2) v->seq_state = 0;
    mutex_unlock(&v->lock);
    if (current) vdec_notify(v, handle, CELL_VDEC_MSG_TYPE_SEQDONE, CELL_OK);
}

/* StartSeq's worker side: reset parser, decoder and worker-owned state. The
 * caller-visible queues were already cleared by cellVdecStartSeq; this runs
 * without any caller waiting on it. */
static void vdec_reset(VdecSlot* v)
{
    avcodec_flush_buffers(v->avctx);
    if (v->parser) {
        av_parser_close(v->parser);
        v->parser = av_parser_init(v->ff_id);
    }
    v->carry_len = 0;
    for (u32 i = 0; i < v->collect_n; i++)
        av_frame_unref(v->collect[i]);
    v->collect_n = 0;
    vdec_clear_spans(v);
}

static void vdec_thread_body(VdecSlot* v, CellVdecHandle handle)
{
    const AVCodec* codec = avcodec_find_decoder(v->ff_id);
    int ok = codec
          && (v->avctx = avcodec_alloc_context3(codec)) != NULL
          && (v->pkt = av_packet_alloc()) != NULL
          && (v->dec_frame = av_frame_alloc()) != NULL;
    if (ok) {
        /* Preserve source ownership through decoder picture reordering. */
        v->avctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
        ok = avcodec_open2(v->avctx, codec, NULL) >= 0
          && (v->parser = av_parser_init(v->ff_id)) != NULL;
    }

    mutex_lock(&v->lock);
    if (!ok) v->fatal = 1;    /* never clear a fatal flag set by Open */
    cond_broadcast(&v->cv);
    mutex_unlock(&v->lock);
    if (!ok) {
        fprintf(stderr, "[cellVdec] handle %u: libavcodec bring-up failed\n",
                handle);
        goto cleanup;
    }

    for (;;) {
        mutex_lock(&v->lock);
        while (!v->closing && v->job_count == 0
               && !v->flush_pending && !v->reset_pending)
            cond_wait(&v->cv, &v->lock);
        if (v->closing) {
            mutex_unlock(&v->lock);
            break;
        }
        /* A reset must precede every AU and drain of its new sequence. */
        if (v->reset_pending) {
            v->reset_pending = 0;
            mutex_unlock(&v->lock);
            vdec_reset(v);
            if (!v->parser) {
                mutex_lock(&v->lock);
                v->fatal = 1;
                mutex_unlock(&v->lock);
                vdec_notify(v, handle, CELL_VDEC_MSG_TYPE_ERROR,
                            (s32)CELL_VDEC_ERROR_FATAL);
                break;
            }
            continue;
        }
        if (v->job_count > 0) {
            VdecAuJob job = v->jobs[v->job_head];
            v->jobs[v->job_head].data = NULL;
            v->job_head = (v->job_head + 1) % VDEC_AU_QUEUE;
            v->job_count--;
            cond_broadcast(&v->cv);               /* room for DecodeAu */
            mutex_unlock(&v->lock);

            vdec_decode_job(v, handle, &job);
            free(job.data);
            continue;
        }
        if (v->flush_pending) {
            const u32 epoch = v->seq_epoch;
            v->flush_pending = 0;
            mutex_unlock(&v->lock);
            vdec_flush(v, handle, epoch);
            continue;
        }
    }

cleanup:
    vdec_clear_spans(v);
    av_parser_close(v->parser);
    avcodec_free_context(&v->avctx);
    av_packet_free(&v->pkt);
    av_frame_free(&v->dec_frame);
    free(v->carry);
    v->carry = NULL;
    v->carry_len = v->carry_cap = 0;
}

#ifdef _WIN32
static unsigned __stdcall vdec_thread_win(void* arg)
{
    VdecSlot* v = (VdecSlot*)arg;
    vdec_thread_body(v, (CellVdecHandle)(v - s_vdec));
    return 0;
}
#else
static void* vdec_thread_posix(void* arg)
{
    VdecSlot* v = (VdecSlot*)arg;
    vdec_thread_body(v, (CellVdecHandle)(v - s_vdec));
    return NULL;
}
#endif

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

/* cellVdecQueryAttr: how much work memory the decoder wants, before Open.
 * CellVdecAttr (guest, big-endian): +0 u32 memSize, +4 u8 cmdDepth,
 * +8 u32 decoderVerUpper, +0xC u32 decoderVerLower.
 * Decoding is host-side, so the guest's buffer is never touched; memSize only
 * has to be something the title can allocate. cmdDepth sizes the title's own
 * AU queue, so it must not be 0. Unimplemented, the call left the struct
 * unfilled and a movie player (Resistance: Fall of Man) gave up and retried
 * forever. */
s32 cellVdecQueryAttr(const CellVdecType* type, void* attr)
{
    u32 ea = (u32)(uintptr_t)attr;
    if (!type || !ea)
        return (s32)CELL_VDEC_ERROR_ARG;
    vm_write32(ea + 0x0, 0x100000);   /* memSize: 1 MB */
    vm_write8 (ea + 0x4, 4);          /* cmdDepth */
    vm_write32(ea + 0x8, 0x00010000); /* decoderVerUpper */
    vm_write32(ea + 0xC, 0);          /* decoderVerLower */
    return CELL_OK;
}

/* cellVdecOpen(type, res, cb, handle) -- FOUR arguments, like cellAdecOpen.
 * cb is a guest pointer to { u32 cbFunc; u32 cbArg; }. Declared with five,
 * `handle` came from r7 instead of r6: the handle was written somewhere
 * random and the title's own handle stayed 0 (Resistance: Fall of Man opened
 * handle 1, then called StartSeq(0)); four Opens later every call was BUSY. */
s32 cellVdecOpen(const CellVdecType* type, const CellVdecResource* res,
                  const CellVdecCb* cb, CellVdecHandle* handle)
{
    u32 res_ea = (u32)(uintptr_t)res;   /* CellVdecResource is guest memory */
    u32 cb_ea = (u32)(uintptr_t)cb;

    if (!type || !handle)
        return (s32)CELL_VDEC_ERROR_ARG;

    const u32 codec_type = vm_read32(GUEST_EA(type));
    enum AVCodecID ff_id = AV_CODEC_ID_NONE;
    switch (codec_type) {
    case CELL_VDEC_CODEC_TYPE_MPEG2:   ff_id = AV_CODEC_ID_MPEG2VIDEO; break;
    case CELL_VDEC_CODEC_TYPE_AVC:
    case CELL_VDEC_CODEC_TYPE_JVT:
    case CELL_VDEC_CODEC_TYPE_MVC:
    case CELL_VDEC_CODEC_TYPE_MVC2:    ff_id = AV_CODEC_ID_H264;       break;
    case CELL_VDEC_CODEC_TYPE_MPEG4:
    case CELL_VDEC_CODEC_TYPE_DIVX:    ff_id = AV_CODEC_ID_MPEG4;      break;
    case CELL_VDEC_CODEC_TYPE_VC1:     ff_id = AV_CODEC_ID_VC1;        break;
    case CELL_VDEC_CODEC_TYPE_DIVX3_11: ff_id = AV_CODEC_ID_MSMPEG4V3; break;
    default:
        printf("[cellVdec] Open: unknown codecType %u\n", codec_type);
        return (s32)CELL_VDEC_ERROR_ARG;
    }
    if (!avcodec_find_decoder(ff_id)) {
        printf("[cellVdec] Open: no libavcodec decoder for codecType %u\n",
               codec_type);
        return (s32)CELL_VDEC_ERROR_ARG;
    }

    for (int i = 0; i < MAX_VDEC; i++) {
        if (!s_vdec[i].in_use) {
            VdecSlot* v = &s_vdec[i];
            memset(v, 0, sizeof(*v));
            v->in_use = 1;
            v->codecType = codec_type;
            v->ff_id = ff_id;
            v->cbFunc = cb_ea ? vm_read32(cb_ea + 0) : 0;
            v->cbArg  = cb_ea ? vm_read32(cb_ea + 4) : 0;
            /* The firmware parks pic items in the work memory the title
             * provided here (CellVdecResource { u32 memAddr, memSize }): one
             * 0x200-byte slot per picture, handed out round-robin by
             * GetPicItem. Without it, fall back to the inject window. */
            if (res_ea) {
                u32 mem_addr = vm_read32(res_ea + 0);
                u32 mem_size = vm_read32(res_ea + 4);
                if (mem_addr && mem_size >= 0x200u) {
                    v->item_base = mem_addr;
                    v->item_slots = mem_size / 0x200u;
                    if (v->item_slots > 4096u) v->item_slots = 4096u;
                }
            }
            mutex_init(&v->lock);
            cond_init(&v->cv);
            for (u32 k = 0; k < VDEC_PIC_QUEUE; k++) {
                v->pics[k].frame = av_frame_alloc();
                if (!v->pics[k].frame) v->fatal = 1;
            }
            for (u32 k = 0; k < VDEC_COLLECT_MAX; k++) {
                v->collect[k] = av_frame_alloc();
                if (!v->collect[k]) v->fatal = 1;
            }
            v->getpic_frame = av_frame_alloc();
            if (!v->getpic_frame) v->fatal = 1;

#include "../../runtime/platform/thread_name.h"   /* ps3_thread_setname */
#ifdef _WIN32
            v->thread = (HANDLE)_beginthreadex(NULL, 0, vdec_thread_win, v,
                                               0, NULL);
            v->thread_live = (v->thread != NULL);
            ps3_thread_setname((uintptr_t)v->thread, "vdec");
#else
            v->thread_live = (pthread_create(&v->thread, NULL,
                                             vdec_thread_posix, v) == 0);
#endif
            if (!v->thread_live) {
                fprintf(stderr, "[cellVdec] Open: worker thread failed\n");
                v->fatal = 1;
            }
            vm_write32((u32)(uintptr_t)handle, (u32)i);
            printf("[cellVdec] Open(codecType=%u) -> handle=%u\n",
                   codec_type, i);
            return CELL_OK;
        }
    }
    return (s32)CELL_VDEC_ERROR_BUSY;
}

s32 cellVdecClose(CellVdecHandle handle)
{
    printf("[cellVdec] Close(handle=%u)\n", handle);

    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;
    VdecSlot* v = &s_vdec[handle];

    mutex_lock(&v->lock);
    v->closing = 1;
    cond_broadcast(&v->cv);
    mutex_unlock(&v->lock);

#ifdef _WIN32
    if (v->thread) {
        WaitForSingleObject(v->thread, INFINITE);
        CloseHandle(v->thread);
        v->thread = NULL;
    }
#else
    if (v->thread_live)
        pthread_join(v->thread, NULL);
#endif
    v->thread_live = 0;

    for (u32 i = 0; i < VDEC_AU_QUEUE; i++)
        free(v->jobs[i].data);
    for (u32 i = 0; i < VDEC_PIC_QUEUE; i++) {
        if (v->pics[i].frame) av_frame_free(&v->pics[i].frame);
    }
    for (u32 i = 0; i < VDEC_COLLECT_MAX; i++) {
        if (v->collect[i]) av_frame_free(&v->collect[i]);
    }
    if (v->getpic_frame) av_frame_free(&v->getpic_frame);
    sws_freeContext(v->sws);
    mutex_destroy(&v->lock);
    cond_destroy(&v->cv);
    memset(v, 0, sizeof(*v));
    return CELL_OK;
}

s32 cellVdecStartSeq(CellVdecHandle handle)
{
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;
    VdecSlot* v = &s_vdec[handle];

    mutex_lock(&v->lock);
    if (v->fatal) {
        mutex_unlock(&v->lock);
        return (s32)CELL_VDEC_ERROR_FATAL;
    }
    if (v->seq_state == 2) {                    /* ending: EndSeq unfinished */
        mutex_unlock(&v->lock);
        return (s32)CELL_VDEC_ERROR_SEQ;
    }
    /* The firmware records a start command and returns immediately; the
     * codec reset runs on the decoder thread. Waiting for it here used to
     * hang the caller forever whenever the worker was blocked on a full
     * picture queue (a consumer that stopped draining between videos). */
    v->seq_epoch++;                             /* stale work is dropped     */
    /* Keep accepted jobs until the worker completes their AUDONE callbacks.
     * Their epoch makes them stale, so they cannot enter the new decoder. */
    for (u32 i = 0; i < VDEC_PIC_QUEUE; i++)    /* queued pictures dropped   */
        av_frame_unref(v->pics[i].frame);
    v->pic_head = v->pic_count = 0;
    v->next_pts = v->next_dts = 0;              /* stamp chains restart      */
    v->seq_state = 1;
    v->reset_pending = 1;                       /* worker resets the codec   */
    cond_broadcast(&v->cv);
    mutex_unlock(&v->lock);
    VDEC_TRACE("StartSeq handle=%u", handle);
    return CELL_OK;
}

s32 cellVdecEndSeq(CellVdecHandle handle)
{
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;
    VdecSlot* v = &s_vdec[handle];

    mutex_lock(&v->lock);
    if (v->fatal) {
        mutex_unlock(&v->lock);
        return (s32)CELL_VDEC_ERROR_FATAL;
    }
    if (v->seq_state != 1) {                    /* not decoding: sequence err */
        mutex_unlock(&v->lock);
        return (s32)CELL_VDEC_ERROR_SEQ;
    }
    v->seq_state = 2;                           /* ending; dormant after SEQDONE */
    v->flush_pending = 1;
    cond_broadcast(&v->cv);
    mutex_unlock(&v->lock);
    VDEC_TRACE("EndSeq handle=%u", handle);
    return CELL_OK;
}

s32 cellVdecDecodeAu(CellVdecHandle handle, s32 mode, const CellVdecAuInfo* auInfo)
{
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;
    if (!auInfo)
        return (s32)CELL_VDEC_ERROR_ARG;
    VdecSlot* v = &s_vdec[handle];

    /* CellVdecAuInfo mixes u32 and u64, so it comes across field by field: a
     * big-endian u64 is high word first, and a word-at-a-time copy would put
     * the two halves the wrong way round on a little-endian host. */
    u32 au_ea = GUEST_EA(auInfo);
    CellVdecAuInfo au;
    au.startAddr = vm_read32(au_ea + (u32)offsetof(CellVdecAuInfo, startAddr));
    au.size      = vm_read32(au_ea + (u32)offsetof(CellVdecAuInfo, size));
    au.pts       = vm_read64(au_ea + (u32)offsetof(CellVdecAuInfo, pts));
    au.dts       = vm_read64(au_ea + (u32)offsetof(CellVdecAuInfo, dts));
    au.userData  = vm_read64(au_ea + (u32)offsetof(CellVdecAuInfo, userData));
    au.codecSpecificData = vm_read64(au_ea + (u32)offsetof(CellVdecAuInfo, codecSpecificData));

    if (au.size == 0 || !au.startAddr)
        return (s32)CELL_VDEC_ERROR_ARG;
    if (mode != CELL_VDEC_DEC_MODE_NORMAL
        && mode != CELL_VDEC_DEC_MODE_B_SKIP
        && mode != CELL_VDEC_DEC_MODE_PB_SKIP)
        return (s32)CELL_VDEC_ERROR_ARG;

    /* CellCodecTimeStamp: upper word 0xFFFFFFFF marks the stamp invalid. */
    const int pts_valid = ((u32)(au.pts >> 32)) != CELL_CODEC_PTS_INVALID;
    const int dts_valid = ((u32)(au.dts >> 32)) != CELL_CODEC_DTS_INVALID;

    mutex_lock(&v->lock);
    if (v->fatal) {
        mutex_unlock(&v->lock);
        return (s32)CELL_VDEC_ERROR_FATAL;
    }
    if (v->seq_state != 1) {                    /* not decoding: sequence err */
        mutex_unlock(&v->lock);
        return (s32)CELL_VDEC_ERROR_SEQ;
    }
    if (v->job_count == VDEC_AU_QUEUE) {
        /* Never block the caller: the firmware answers BUSY (a retryable
         * not-an-error) when its AU queue is full. */
        mutex_unlock(&v->lock);
        VDEC_TRACE("DecodeAu handle=%u BUSY", handle);
        return (s32)CELL_VDEC_ERROR_BUSY;
    }

    /* Copy the AU out of guest memory, padded the way libavcodec requires.
     * After this point nothing in the decoder references the guest's buffer,
     * so the title may reuse it as soon as DecodeAu returns (and so before
     * AUDONE can fire for it). */
    VdecAuJob* job = &v->jobs[(v->job_head + v->job_count) % VDEC_AU_QUEUE];
    job->data = (u8*)malloc((size_t)au.size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!job->data) {
        mutex_unlock(&v->lock);
        return (s32)CELL_VDEC_ERROR_FATAL;
    }
    vm_memcpy_from(job->data, au.startAddr, au.size);
    memset(job->data + au.size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    job->size = au.size;
    job->pts = au.pts;
    job->dts = au.dts;
    job->pts_valid = pts_valid;
    job->dts_valid = dts_valid;
    job->userData = au.userData;
    job->epoch = v->seq_epoch;
    job->mode = mode;
    v->job_count++;
    cond_broadcast(&v->cv);
    mutex_unlock(&v->lock);
    VDEC_TRACE("DecodeAu handle=%u size=%u pts=%llu mode=%d",
               handle, au.size, pts_valid ? (unsigned long long)au.pts : 0ull,
               mode);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Guest-resident pic items. cellVdecGetPicItem hands the title a POINTER to a
 * CellVdecPicItem, so it has to live in guest memory, big-endian; writing
 * &host_struct through the guest's out-pointer crashed Resistance: Fall of Man
 * inside its PICOUT callback.
 *
 * The firmware parks items in the work memory the title handed cellVdecOpen
 * (CellVdecResource::memAddr): one 0x200-byte slot per picture, handed out
 * round-robin. The slot is { CellVdecPicItem at +0, codec info at +0x80 }.
 * When no work memory was provided, one permanent slot per decoder lives in
 * the HLE inject window after cellAdec's scratch (VM_HLE_INJECT_BASE +
 * 0x40000..0x50000). A title that follows item->picInfo keeps its pointer
 * valid until the slot wraps -- a title that re-reads it after
 * VDEC_ITEM_SLOTS more pictures sees the newest picture there instead.
 *
 * Field semantics below follow the firmware (verified against a current
 * reference emulator implementation): startAddr is the "no address for
 * picture" marker 0x123 (NOT the AU address); size is the decoded frame's
 * byte size, 128-aligned (NOT the AU size); auPts[0]/auDts[0] come from the
 * presentation stamp chains and are always valid, [1] is the invalid marker;
 * attr reports the decode mode. Titles that keyed off the AU address (e.g.
 * to throttle their demuxer by AU) misread the old values.
 *
 * Guest CellVdecPicItem: +0x00 codecType, +0x04 startAddr, +0x08 size,
 * +0x0C u8 auNum, +0x10 auPts[2] {upper,lower}, +0x20 auDts[2],
 * +0x30 u64 auUserData[2], +0x40 status, +0x44 attr (0 normal, 1 skipped),
 * +0x48 picInfo. */
static u32 vdec_write_item(VdecSlot* v, CellVdecHandle handle, const VdecPic* p)
{
    const u32 base = v->item_base ? v->item_base : VDEC_ITEM_EA(handle);
    const u32 slots = v->item_base ? v->item_slots : 1;
    const u32 it = base + (v->item_bias % slots) * 0x200u;
    const u32 info = it + 0x80u;
    const AVFrame* f = p->frame;

    for (u32 o = 0; o < 0x200; o += 4) vm_write32(it + o, 0);
    vm_write32(it + 0x00, v->codecType);
    vm_write32(it + 0x04, 0x00000123u);               /* no address for picture */
    {
        int bs = av_image_get_buffer_size(f->format, f->width, f->height, 1);
        if (bs < 0) bs = 0;
        vm_write32(it + 0x08, ((u32)bs + 127u) & ~127u);
    }
    vm_write8 (it + 0x0C, 1);                          /* one AU per picture */
    vm_write64(it + 0x10, p->pts);                     /* auPts[0]: chain stamp */
    vm_write64(it + 0x18, ~0ull);                      /* auPts[1]: unused */
    vm_write64(it + 0x20, p->dts);                     /* auDts[0]: chain stamp */
    vm_write64(it + 0x28, ~0ull);                      /* auDts[1]: unused */
    vm_write64(it + 0x30, p->userData);                /* auUserData[0] */
    vm_write32(it + 0x40, 0);                          /* status: OK */
    vm_write32(it + 0x44, p->attr);
    vm_write32(it + 0x48, info);

    /* Codec info block: everything but the leading fields stays zero, like
     * the firmware's report. Titles size and place video from these. */
    if (v->codecType == CELL_VDEC_CODEC_TYPE_AVC
        || v->codecType == CELL_VDEC_CODEC_TYPE_JVT
        || v->codecType == CELL_VDEC_CODEC_TYPE_MVC
        || v->codecType == CELL_VDEC_CODEC_TYPE_MVC2) {
        /* CellVdecAvcInfo: +0/+2 u16 size, +4/+5 pictureType[2], +6 idrFlag,
         * +7 aspect_ratio_idc, +8/+0xA sar_height/sar_width. The firmware
         * reports idr=false, SAR unspecified and pictureType[1] unknown. */
        u8 pct = 3;                                    /* UNKNOWN */
        if (f->pict_type == AV_PICTURE_TYPE_I) pct = 0;
        else if (f->pict_type == AV_PICTURE_TYPE_P) pct = 1;
        else if (f->pict_type == AV_PICTURE_TYPE_B) pct = 2;
        vm_write16(info + 0, (u16)f->width);
        vm_write16(info + 2, (u16)f->height);
        vm_write8(info + 4, pct);
        vm_write8(info + 5, 3);                        /* UNKNOWN */
        vm_write8(info + 6, 0);                        /* idrPictureFlag: off */
        vm_write8(info + 7, 0);                        /* SAR unspecified */
        vm_write16(info + 8, 0);                       /* sar_height */
        vm_write16(info + 0xA, 0);                     /* sar_width */
    } else {
        /* CellVdecMpeg2Info (also a sane zero block for MPEG-4/DivX, whose
         * CellVdecDivxInfo starts with pictureType then the same u16 sizes).
         * The firmware reports a zeroed block with: coded size,
         * aspect_ratio_information = 1 (SAR 1:1), the frame-rate code,
         * progressive_sequence = 1, low_delay = 1, video_format =
         * unspecified, colour_description = 0, picture_coding_type[1] = 0
         * (forbidden) and picture_structure = 3/3 (frame). Display size and
         * progressive_frame stay zero. */
        u8 pct = 0;
        if (f->pict_type == AV_PICTURE_TYPE_I) pct = 1;
        else if (f->pict_type == AV_PICTURE_TYPE_P) pct = 2;
        else if (f->pict_type == AV_PICTURE_TYPE_B) pct = 3;
        vm_write16(info + 0, (u16)f->width);
        vm_write16(info + 2, (u16)f->height);
        vm_write8(info + 4, 1);                        /* SAR 1:1 */
        vm_write8(info + 5, (u8)p->frc);
        vm_write8(info + 6, 1);                        /* progressive_sequence */
        vm_write8(info + 7, 1);                        /* low_delay */
        vm_write8(info + 8, 5);                        /* video_format: unspec */
        vm_write8(info + 9, 0);                        /* colour_description */
        vm_write8(info + 0x12, pct);
        vm_write8(info + 0x13, 0);                     /* forbidden */
        vm_write8(info + 0x14, 3);                     /* FRAME picture */
        vm_write8(info + 0x15, 3);
    }
    return it;
}

/* cellVdecGetPicItem(handle, &item): the next not-yet-described picture's
 * item, without consuming it; cellVdecGetPicture is what takes pictures off
 * the queue. A picture's item is handed out exactly once -- peeks while no
 * undescribed picture remains answer EMPTY (firmware behavior; titles use
 * this as their "queue empty" poll). */
s32 cellVdecGetPicItem(CellVdecHandle handle, const CellVdecPicItem** picItem)
{
    u32 out = (u32)(uintptr_t)picItem;
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use || !out)
        return (s32)CELL_VDEC_ERROR_ARG;
    VdecSlot* v = &s_vdec[handle];

    mutex_lock(&v->lock);
    VdecPic* p = NULL;
    for (u32 i = 0; i < v->pic_count; i++) {
        VdecPic* c = &v->pics[(v->pic_head + i) % VDEC_PIC_QUEUE];
        if (!c->item_taken) { p = c; break; }
    }
    if (!p) {
        mutex_unlock(&v->lock);
        VDEC_TRACE("GetPicItem handle=%u EMPTY", handle);
        return (s32)CELL_VDEC_ERROR_EMPTY;
    }
    p->item_taken = 1;
    const u32 it = vdec_write_item(v, handle, p);
    const u64 pts = p->pts;
    v->item_bias++;
    mutex_unlock(&v->lock);
    vm_write32(out, it);
    VDEC_TRACE("GetPicItem handle=%u item=0x%08X pts=%llu",
               handle, it, (unsigned long long)pts);
    return CELL_OK;
}

/* cellVdecGetPicture(handle, format, outBuff): convert the next picture out
 * in the requested format and consume it. format is a guest CellVdecPicFormat
 * { u32 formatType; u32 colorMatrixType; u8 alpha; }. A null outBuff consumes
 * the picture without converting it (the documented "skip" behavior). A bad
 * format leaves the queue untouched. */
s32 cellVdecGetPicture(CellVdecHandle handle, const void* format, void* outBuff)
{
    u32 fmt_ea = (u32)(uintptr_t)format, dst = (u32)(uintptr_t)outBuff;
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use || !fmt_ea)
        return (s32)CELL_VDEC_ERROR_ARG;
    VdecSlot* v = &s_vdec[handle];

    const u32 type   = vm_read32(fmt_ea + 0);
    const u32 matrix = vm_read32(fmt_ea + 4);
    const u8  alpha  = vm_read8(fmt_ea + 8);
    if (type > CELL_VDEC_PICFMT_YUV420_PLANAR)
        return (s32)CELL_VDEC_ERROR_ARG;
    if (matrix != CELL_VDEC_COLOR_MATRIX_BT601
        && matrix != CELL_VDEC_COLOR_MATRIX_BT709)
        return (s32)CELL_VDEC_ERROR_ARG;

    mutex_lock(&v->lock);
    if (v->pic_count == 0) {
        mutex_unlock(&v->lock);
        return (s32)CELL_VDEC_ERROR_EMPTY;
    }
    VdecPic* p = &v->pics[v->pic_head];
    if (!dst) {
        /* Skip: consume without output. */
        av_frame_unref(p->frame);
        v->pic_head = (v->pic_head + 1) % VDEC_PIC_QUEUE;
        v->pic_count--;
        cond_broadcast(&v->cv);
        mutex_unlock(&v->lock);
        return CELL_OK;
    }
    av_frame_unref(v->getpic_frame);
    av_frame_move_ref(v->getpic_frame, p->frame);
    v->pic_head = (v->pic_head + 1) % VDEC_PIC_QUEUE;
    v->pic_count--;
    cond_broadcast(&v->cv);
    mutex_unlock(&v->lock);

    AVFrame* f = v->getpic_frame;
    if (f->width <= 0 || f->height <= 0
        || (u32)f->width * (u32)f->height > 4096u * 2160u)
        return (s32)CELL_VDEC_ERROR_PIC;

    enum AVPixelFormat dst_fmt;
    u32 stride;
    switch (type) {
    case CELL_VDEC_PICFMT_ARGB32_ILV:  dst_fmt = AV_PIX_FMT_ARGB;     stride = (u32)f->width * 4; break;
    case CELL_VDEC_PICFMT_RGBA32_ILV:  dst_fmt = AV_PIX_FMT_RGBA;     stride = (u32)f->width * 4; break;
    case CELL_VDEC_PICFMT_UYVY422_ILV: dst_fmt = AV_PIX_FMT_UYVY422; stride = (u32)f->width * 2; break;
    default:                           dst_fmt = AV_PIX_FMT_YUV420P;  stride = (u32)f->width;     break;
    }

    /* The firmware converts with a point (nearest-neighbour) filter; bilinear
     * smears chroma edges on menu overlays built from video frames. */
    v->sws = sws_getCachedContext(v->sws, f->width, f->height,
                                  (enum AVPixelFormat)f->format,
                                  f->width, f->height, dst_fmt,
                                  SWS_POINT, NULL, NULL, NULL);
    if (!v->sws)
        return (s32)CELL_VDEC_ERROR_PIC;
    const int cs = (matrix == CELL_VDEC_COLOR_MATRIX_BT709)
                     ? SWS_CS_ITU709 : SWS_CS_DEFAULT;
    const int full_src = (f->color_range == AVCOL_RANGE_JPEG);
    sws_setColorspaceDetails(v->sws, sws_getCoefficients(cs), full_src,
                             sws_getCoefficients(cs), 0, 0, 1 << 16, 1 << 16);

    if (type == CELL_VDEC_PICFMT_YUV420_PLANAR) {
        const u32 w = (u32)f->width, h = (u32)f->height;
        const u32 c_off = w * h;                     /* U plane; V at 5/4 */
        u8* planes[4] = { vm_ptr8(dst), vm_ptr8(dst + c_off),
                          vm_ptr8(dst + c_off * 5 / 4), NULL };
        int strides[4] = { (int)w, (int)(w / 2), (int)(w / 2), 0 };
        if (sws_scale(v->sws, (const uint8_t* const*)f->data, f->linesize,
                      0, f->height, planes, strides) < 0)
            return (s32)CELL_VDEC_ERROR_PIC;
    } else {
        u8* planes[4] = { vm_ptr8(dst), NULL, NULL, NULL };
        int strides[4] = { (int)stride, 0, 0, 0 };
        if (sws_scale(v->sws, (const uint8_t* const*)f->data, f->linesize,
                      0, f->height, planes, strides) < 0)
            return (s32)CELL_VDEC_ERROR_PIC;
        /* libswscale leaves alpha unspecified; the title asked for one value. */
        if (type == CELL_VDEC_PICFMT_ARGB32_ILV
            || type == CELL_VDEC_PICFMT_RGBA32_ILV) {
            const u32 ai = (type == CELL_VDEC_PICFMT_ARGB32_ILV) ? 0 : 3;
            u8* base = vm_ptr8(dst);
            for (int y = 0; y < f->height; y++) {
                u8* row = base + (u32)y * stride + ai;
                for (int x = 0; x < f->width; x++)
                    row[(u32)x * 4] = alpha;
            }
        }
    }
    VDEC_TRACE("GetPicture handle=%u type=%u matrix=%u alpha=%u dst=0x%08X",
               handle, type, matrix, alpha, dst);
    return CELL_OK;
}

s32 cellVdecSetFrameRate(CellVdecHandle handle, u32 frameRateCode)
{
    if (handle >= MAX_VDEC || !s_vdec[handle].in_use)
        return (s32)CELL_VDEC_ERROR_ARG;
    /* Only the eight CellVdecFrameRate codes 0x80..0x87 exist; the firmware
     * masks to 0x87 and rejects anything outside the 0x80 block. The stored
     * override drives the presentation stamp chains and the item's
     * frame_rate_code. */
    if ((frameRateCode & 0xf8u) != 0x80u)
        return (s32)CELL_VDEC_ERROR_ARG;
    VdecSlot* v = &s_vdec[handle];
    mutex_lock(&v->lock);
    v->frameRateCode = frameRateCode;
    v->frc_set = frameRateCode & 0x87u;
    mutex_unlock(&v->lock);
    return CELL_OK;
}
