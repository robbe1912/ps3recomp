/* spu_job_dump.inc - opt-in dump of the jobs of one SPU image.
 *
 * Include once, in spu_channels.c. Prototypes are in spu_job_dump.h. The
 * definitions live in a file that each build already links, so the two hooks
 * add no link dependency to the headers that call them.
 *
 * Switch
 *   SPU_JOB_DUMP=<image>:<entry>[:<max jobs>[:<every n-th job>]]:<directory>
 *   image      decimal id of the job module image
 *   entry      local store address of the job entry, decimal or 0x hex
 *   max jobs   stop after this number of jobs, default 64
 *   every      dump every n-th entry of a context, default 1
 *   directory  output directory; it is made when it does not exist
 *   Example: SPU_JOB_DUMP=210:0x16270:64:1:E:/captures/pvs
 * Without the switch nothing runs and no file is written.
 *
 * What a job is
 *   A job starts when an admission test examines the entry address inside the
 *   span of the image. Each later admission test that examines an address
 *   outside the span is an exit point; the dump keeps the local store of the
 *   last exit point. A job closes at the next entry, when an admission test
 *   examines the select-workload address of the policy module, or at the exit
 *   of the process. DMA records between the last exit point and the close
 *   come from the policy module.
 *
 * File: <directory>/job_<sequence>.bin. All fields are little-endian.
 *   File header, 192 bytes
 *     0x00 u32 magic 0x55444A53 ("SJDU")      0x04 u32 version 1
 *     0x08 u32 header size 192                0x0C u32 job sequence number
 *     0x10 u32 image of the switch            0x14 u32 entry of the switch
 *     0x18 u32 image id of the context        0x1C u32 SPU id of the context
 *     0x20 u32 host thread id                 0x24 u32 count of resident spans
 *     0x28 u64 context pointer                0x30 u32 r3 words 0..3 at entry
 *     0x40 u32 r4 words 0..3 at entry          0x50 u64 entry time in ns
 *     0x58 u64 close time in ns               0x60 u64 time of the last exit
 *     0x68 u32 address of the last exit point  0x6C u32 close reason
 *     0x70 4 x (u32 image id, u32 base, u32 size): resident spans of the
 *          context at entry
 *     0xA0 32 zero bytes
 *   Record: u32 tag, u32 payload size, u32 record number, u32 zero, payload.
 *   The record number is 0 for the entry local store record, then 1, 2, ...
 *   for the DMA records and exit records in time order.
 *   The payload size is a multiple of 16.
 *   Tag 1 DMA, payload 64 bytes, then the bytes, then zero padding
 *     0x00 u32 1 for a get                    0x04 u32 MFC command word
 *     0x08 u32 local store address            0x0C u32 guest address
 *     0x10 u32 size                           0x14 u32 address in local
 *                                               store of the issuing code
 *     0x18 u32 image id of the context        0x1C u32 exit points before
 *                                               this record
 *     0x20 u32 1 when the issuing code is inside the dumped span
 *     0x24 u32 flags, bit 0: the bytes are not stored
 *     0x28 u64 time in ns                     0x30 u32 DMA record number
 *     0x34 u32 zero                           0x38 8 zero bytes
 *   Tag 2 exit point, payload 32 bytes
 *     0x00 u32 exit number, from 1            0x04 u32 examined address
 *     0x08 u32 r0 word 0                      0x0C u32 r1 word 0
 *     0x10 u32 r3 word 0                      0x14 u32 r4 word 0
 *     0x18 u64 time in ns
 *   Tag 3 entry local store, payload 262144 bytes
 *   Tag 4 exit local store, payload 32 bytes as tag 2, then 262144 bytes
 *   Tag 5 end, payload 64 bytes
 *     0x00 u32 DMA records                    0x04 u32 DMA records without bytes
 *     0x08 u32 DMA records not stored         0x0C u32 exit points
 *     0x10 u32 count of the DMA records and exit records
 *     0x14 u32 close reason                   0x18 u64 get bytes
 *     0x20 u64 put bytes                      0x28 u32 get records
 *     0x2C u32 put records                    0x30 u64 job time in ns
 *     0x38 8 zero bytes
 *   Close reason: 1 next entry, 2 select-workload address, 3 exit of the
 *   process, 4 the slot of the job was needed for another context, 5 the
 *   dump turned off.
 *   Record order: tag 3, then tags 1 and 2 in time order, then tag 4, tag 5.
 *
 * Limits
 *   - A transfer that the DMA engine rejects moves no bytes and has no record.
 *   - At most 32 MiB of DMA bytes per job are stored. Later DMA records keep
 *     their header with flag bit 0.
 *   - Lock-line atomics (GETLLAR, PUTLLC) and mailbox writes are not recorded.
 */

#include "spu_job_dump.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

int g_spu_job_dump;

#define JD_MAGIC        0x55444A53u
#define JD_VERSION      1u
#define JD_HEADER_SIZE  192u
#define JD_REC_HEAD     16u
#define JD_DMA_HEAD     64u
#define JD_EXIT_HEAD    32u
#define JD_END_HEAD     64u
#define JD_SLOTS        64u
#define JD_DMA_CAP      (32u * 1024u * 1024u)
#define JD_DEFAULT_MAX  64u
#define JD_PATH_SIZE    1024u
#define JD_DMA_NO_DATA  1u

enum {
    JD_REC_DMA = 1, JD_REC_EXIT_POINT = 2, JD_REC_ENTRY_LS = 3,
    JD_REC_EXIT_LS = 4, JD_REC_END = 5,
    JD_CLOSE_NEXT_ENTRY = 1, JD_CLOSE_SELECT_WORKLOAD = 2,
    JD_CLOSE_PROCESS_EXIT = 3, JD_CLOSE_REUSED = 4, JD_CLOSE_DUMP_OFF = 5
};

typedef struct {
    uint64_t thread;        /* owner; 0 when the slot is free */
    const spu_context* ctx;
    uint32_t seq;
    int active;
    int in_span;            /* the last admission test was inside the span */
    uint32_t exits, exit_pc, exit_r0, exit_r1, exit_r3, exit_r4;
    uint32_t entry_r3[4], entry_r4[4], ctx_image, spu_id;
    uint32_t resident_n;
    uint32_t resident[4][3];
    uint32_t records, dma_n, dma_no_data, dma_lost;
    uint32_t get_n, put_n;
    uint64_t get_bytes, put_bytes;
    uint64_t seen;          /* entry tests of this context */
    uint64_t t_entry, t_exit;
    uint8_t* entry_ls;
    uint8_t* exit_ls;
    uint8_t* buf;
    size_t buf_size, buf_cap;
} jd_slot;

static jd_slot s_jd_slots[JD_SLOTS];
static struct {
    uint32_t image, entry, max, every;
    char dir[JD_PATH_SIZE];
} s_jd_cfg;
static uint32_t s_jd_started, s_jd_written;
static int s_jd_off;

void spu_job_dump_flush(void);
static void jd_turn_off(void);
static void jd_write_job(jd_slot* s, int reason);

#ifdef _WIN32
static CRITICAL_SECTION s_jd_lock;
static INIT_ONCE s_jd_lock_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK jd_lock_create(PINIT_ONCE once, PVOID param, PVOID* context)
{
    (void)once; (void)param; (void)context;
    InitializeCriticalSection(&s_jd_lock);
    return TRUE;
}
static void jd_lock_take(void)
{
    InitOnceExecuteOnce(&s_jd_lock_once, jd_lock_create, NULL, NULL);
    EnterCriticalSection(&s_jd_lock);
}
static void jd_lock_give(void) { LeaveCriticalSection(&s_jd_lock); }
#else
static pthread_mutex_t s_jd_lock = PTHREAD_MUTEX_INITIALIZER;
static void jd_lock_take(void) { pthread_mutex_lock(&s_jd_lock); }
static void jd_lock_give(void) { pthread_mutex_unlock(&s_jd_lock); }
#endif

static uint64_t jd_thread_id(void)
{
#ifdef _WIN32
    return (uint64_t)GetCurrentThreadId();
#else
    pthread_t t = pthread_self();
    uint64_t v = 0;
    memcpy(&v, &t, sizeof(t) < sizeof(v) ? sizeof(t) : sizeof(v));
    return v;
#endif
}

static uint64_t jd_now_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart * 1e9 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static void jd_put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void jd_put64(uint8_t* p, uint64_t v)
{
    jd_put32(p, (uint32_t)v);
    jd_put32(p + 4, (uint32_t)(v >> 32));
}

/* Decimal or 0x hex number at the start of `s`. Returns the end, or NULL. */
static const char* jd_number(const char* s, uint32_t* value)
{
    char* end;
    int hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    const char* digits = s + (hex ? 2 : 0);
    unsigned long long v = strtoull(digits, &end, hex ? 16 : 10);
    if (end == digits || v > 0xFFFFFFFFull) return NULL;
    *value = (uint32_t)v;
    return end;
}

static void jd_mkdir_one(char* path)
{
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0777);
#endif
}

static void jd_mkdir(const char* path)
{
    char part[JD_PATH_SIZE];
    size_t n = strlen(path), i;
    if (n >= sizeof(part)) return;
    memcpy(part, path, n + 1);
    for (i = 1; i < n; i++) {
        char c = part[i];
        if (c != '/' && c != '\\') continue;
        /* Keep the separator of a drive prefix, as in "E:/dir". */
        if (i == 2 && part[1] == ':') continue;
        part[i] = 0;
        jd_mkdir_one(part);
        part[i] = c;
    }
    jd_mkdir_one(part);
}

/* Lock taken. Closes the open jobs, then stops the dump. */
static void jd_turn_off(void)
{
    unsigned i;
    g_spu_job_dump = -1;
    if (s_jd_off) return;
    s_jd_off = 1;
    for (i = 0; i < JD_SLOTS; i++)
        if (s_jd_slots[i].active) jd_write_job(&s_jd_slots[i], JD_CLOSE_DUMP_OFF);
    fprintf(stderr, "[job-dump] off: jobs started %u, files written %u\n",
            s_jd_started, s_jd_written);
    fflush(stderr);
}

/* Runs once, with the lock taken. */
static void jd_init(void)
{
    static int registered;
    const char* e = getenv("SPU_JOB_DUMP");
    const char* p;
    uint32_t image, entry, max = JD_DEFAULT_MAX, every = 1, field;
    if (!e || !*e) { g_spu_job_dump = -1; return; }
    p = jd_number(e, &image);
    if (!p || *p != ':' || !image) goto invalid;
    p = jd_number(p + 1, &entry);
    if (!p || *p != ':' || (entry & 3u) || entry >= SPU_LS_SIZE) goto invalid;
    p++;
    /* Two optional decimal fields. Each must end with a colon, so a
     * directory name does not read as a number. */
    for (field = 0; field < 2; field++) {
        uint32_t v;
        const char* q = (*p >= '0' && *p <= '9') ? jd_number(p, &v) : NULL;
        if (!q || *q != ':') break;
        if (field == 0) max = v; else every = v;
        p = q + 1;
    }
    if (!max || !every || strlen(p) >= sizeof(s_jd_cfg.dir)) goto invalid;
    s_jd_cfg.image = image;
    s_jd_cfg.entry = entry;
    s_jd_cfg.max = max;
    s_jd_cfg.every = every;
    memcpy(s_jd_cfg.dir, p, strlen(p) + 1);
    jd_mkdir(s_jd_cfg.dir);
    g_spu_job_dump = 1;
    fprintf(stderr, "[job-dump] on: image=%u entry=0x%05X max=%u every=%u dir=%s\n",
            image, entry, max, every, s_jd_cfg.dir);
    fflush(stderr);
    if (!registered) {
        registered = 1;
        atexit(spu_job_dump_flush);
    }
    return;
invalid:
    fprintf(stderr, "SPU_JOB_DUMP must be <image>:<entry>[:<max jobs>[:<every n-th job>]]:"
                    "<directory>, got '%s'\n", e);
    exit(2);
}

/* The hooks call this while the switch state is 0. */
static void jd_resolve(void)
{
    if (g_spu_job_dump) return;
    jd_lock_take();
    if (!g_spu_job_dump) jd_init();
    jd_lock_give();
}

/* Slot of the context, or NULL when the context has none. Lock taken. */
static jd_slot* jd_slot_find(const spu_context* ctx)
{
    uint64_t thread = jd_thread_id();
    unsigned i;
    for (i = 0; i < JD_SLOTS; i++)
        if (s_jd_slots[i].thread == thread && s_jd_slots[i].ctx == ctx)
            return &s_jd_slots[i];
    return NULL;
}

/* A free slot, or the slot of the oldest job. Lock taken. */
static jd_slot* jd_slot_claim(const spu_context* ctx)
{
    jd_slot* oldest = &s_jd_slots[0];
    unsigned i;
    for (i = 0; i < JD_SLOTS; i++) {
        jd_slot* s = &s_jd_slots[i];
        if (!s->thread) { oldest = s; break; }
        if (s->t_entry < oldest->t_entry) oldest = s;
    }
    if (!oldest->entry_ls) {
        oldest->entry_ls = (uint8_t*)malloc(SPU_LS_SIZE);
        oldest->exit_ls = (uint8_t*)malloc(SPU_LS_SIZE);
        if (!oldest->entry_ls || !oldest->exit_ls) {
            free(oldest->entry_ls); free(oldest->exit_ls);
            oldest->entry_ls = oldest->exit_ls = NULL;
            fprintf(stderr, "[job-dump] out of memory; the dump is off\n");
            jd_turn_off();
            return NULL;
        }
    }
    if (oldest->active) jd_write_job(oldest, JD_CLOSE_REUSED);
    oldest->thread = jd_thread_id();
    oldest->ctx = ctx;
    oldest->seen = 0;
    return oldest;
}

static int jd_reserve(jd_slot* s, size_t need)
{
    size_t cap = s->buf_cap ? s->buf_cap : 65536u;
    uint8_t* grown;
    if (need <= s->buf_cap) return 1;
    while (cap < need) cap *= 2;
    grown = (uint8_t*)realloc(s->buf, cap);
    if (!grown) return 0;
    s->buf = grown;
    s->buf_cap = cap;
    return 1;
}

static void jd_head(uint8_t* p, uint32_t tag, uint32_t payload, uint32_t number)
{
    jd_put32(p, tag);
    jd_put32(p + 4, payload);
    jd_put32(p + 8, number);
    jd_put32(p + 12, 0);
}

/* Index of the resident span of the dumped image that holds `pc`, or -1. */
static int jd_span(const spu_context* ctx, uint32_t pc)
{
    unsigned i;
    for (i = 0; i < sizeof(ctx->resident_code) / sizeof(ctx->resident_code[0]); i++) {
        uint32_t base = ctx->resident_code[i].lsa, size = ctx->resident_code[i].size;
        if ((uint32_t)ctx->resident_code[i].image_id == s_jd_cfg.image && size &&
            pc >= base && pc - base < size)
            return (int)i;
    }
    return -1;
}

/* Lock taken. */
static void jd_start(jd_slot* s, const spu_context* ctx)
{
    unsigned i;
    if (s_jd_started >= s_jd_cfg.max) { jd_turn_off(); return; }
    if (s->seen++ % s_jd_cfg.every) return;
    s->seq = ++s_jd_started;
    s->active = 1;
    s->in_span = 1;
    s->exits = s->exit_pc = s->exit_r0 = s->exit_r1 = s->exit_r3 = s->exit_r4 = 0;
    s->records = s->dma_n = s->dma_no_data = s->dma_lost = 0;
    s->get_n = s->put_n = 0;
    s->get_bytes = s->put_bytes = 0;
    s->buf_size = 0;
    s->t_exit = 0;
    s->t_entry = jd_now_ns();
    s->ctx_image = (uint32_t)ctx->image_id;
    s->spu_id = ctx->spu_id;
    for (i = 0; i < 4; i++) {
        s->entry_r3[i] = ctx->gpr[3]._u32[i];
        s->entry_r4[i] = ctx->gpr[4]._u32[i];
    }
    s->resident_n = 0;
    for (i = 0; i < 4; i++) {
        if (!ctx->resident_code[i].image_id || !ctx->resident_code[i].size) continue;
        s->resident[s->resident_n][0] = (uint32_t)ctx->resident_code[i].image_id;
        s->resident[s->resident_n][1] = ctx->resident_code[i].lsa;
        s->resident[s->resident_n][2] = ctx->resident_code[i].size;
        s->resident_n++;
    }
    memcpy(s->entry_ls, ctx->ls, SPU_LS_SIZE);
}

/* Lock taken. */
static void jd_exit_point(jd_slot* s, const spu_context* ctx, uint32_t pc)
{
    uint8_t* p;
    size_t need = s->buf_size + JD_REC_HEAD + JD_EXIT_HEAD;
    s->exits++;
    s->exit_pc = pc;
    s->exit_r0 = ctx->gpr[0]._u32[0];
    s->exit_r1 = ctx->gpr[1]._u32[0];
    s->exit_r3 = ctx->gpr[3]._u32[0];
    s->exit_r4 = ctx->gpr[4]._u32[0];
    s->t_exit = jd_now_ns();
    s->in_span = 0;
    memcpy(s->exit_ls, ctx->ls, SPU_LS_SIZE);
    if (!jd_reserve(s, need)) { s->dma_lost++; return; }
    p = s->buf + s->buf_size;
    jd_head(p, JD_REC_EXIT_POINT, JD_EXIT_HEAD, ++s->records);
    jd_put32(p + JD_REC_HEAD, s->exits);
    jd_put32(p + JD_REC_HEAD + 4, pc);
    jd_put32(p + JD_REC_HEAD + 8, s->exit_r0);
    jd_put32(p + JD_REC_HEAD + 12, s->exit_r1);
    jd_put32(p + JD_REC_HEAD + 16, s->exit_r3);
    jd_put32(p + JD_REC_HEAD + 20, s->exit_r4);
    jd_put64(p + JD_REC_HEAD + 24, s->t_exit);
    s->buf_size = need;
}

/* Lock taken. */
static void jd_append_dma(jd_slot* s, const spu_context* ctx, uint32_t lsa,
                          uint32_t ea, uint32_t size, uint32_t cmd, int is_get,
                          const uint8_t* bytes, int in_span)
{
    uint32_t flags = 0, padded = (size + 15u) & ~15u;
    uint8_t* p;
    size_t need;
    s->dma_n++;
    if (s->get_bytes + s->put_bytes + size > JD_DMA_CAP) flags |= JD_DMA_NO_DATA;
    need = s->buf_size + JD_REC_HEAD + JD_DMA_HEAD + (flags ? 0u : padded);
    if (!jd_reserve(s, need)) { s->dma_lost++; return; }
    if (flags) s->dma_no_data++;
    if (is_get) { s->get_n++; s->get_bytes += size; }
    else { s->put_n++; s->put_bytes += size; }
    p = s->buf + s->buf_size;
    jd_head(p, JD_REC_DMA, JD_DMA_HEAD + (flags ? 0u : padded), ++s->records);
    jd_put32(p + JD_REC_HEAD, is_get ? 1u : 0u);
    jd_put32(p + JD_REC_HEAD + 4, cmd);
    jd_put32(p + JD_REC_HEAD + 8, lsa);
    jd_put32(p + JD_REC_HEAD + 12, ea);
    jd_put32(p + JD_REC_HEAD + 16, size);
    jd_put32(p + JD_REC_HEAD + 20, (uint32_t)ctx->pc & SPU_LS_MASK);
    jd_put32(p + JD_REC_HEAD + 24, (uint32_t)ctx->image_id);
    jd_put32(p + JD_REC_HEAD + 28, s->exits);
    jd_put32(p + JD_REC_HEAD + 32, in_span ? 1u : 0u);
    jd_put32(p + JD_REC_HEAD + 36, flags);
    jd_put64(p + JD_REC_HEAD + 40, jd_now_ns());
    jd_put32(p + JD_REC_HEAD + 48, s->dma_n);
    jd_put32(p + JD_REC_HEAD + 52, 0);
    if (!flags) {
        p += JD_REC_HEAD + JD_DMA_HEAD;
        memcpy(p, bytes, size);
        memset(p + size, 0, padded - size);
    }
    s->buf_size = need;
}

static int jd_write(FILE* f, const void* data, size_t size)
{
    return !size || fwrite(data, 1, size, f) == size;
}

static int jd_write_block(FILE* f, uint32_t tag, uint32_t number,
                          const uint8_t* head, uint32_t head_size,
                          const uint8_t* ls)
{
    uint8_t h[JD_REC_HEAD];
    jd_head(h, tag, head_size + (ls ? SPU_LS_SIZE : 0u), number);
    return jd_write(f, h, sizeof(h)) && jd_write(f, head, head_size) &&
           jd_write(f, ls, ls ? SPU_LS_SIZE : 0u);
}

/* Lock taken. Writes the file of the job. The slot is free for a new job. */
static void jd_write_job(jd_slot* s, int reason)
{
    char path[JD_PATH_SIZE + 32];
    FILE* f;
    uint8_t h[JD_HEADER_SIZE], x[JD_EXIT_HEAD], e[JD_END_HEAD];
    uint64_t t_close = jd_now_ns();
    uint32_t number = s->records + 1, buffer_records = s->records;
    unsigned i;
    int ok;
    if (!s->active) return;
    memset(h, 0, sizeof(h));
    jd_put32(h, JD_MAGIC);
    jd_put32(h + 4, JD_VERSION);
    jd_put32(h + 8, JD_HEADER_SIZE);
    jd_put32(h + 12, s->seq);
    jd_put32(h + 16, s_jd_cfg.image);
    jd_put32(h + 20, s_jd_cfg.entry);
    jd_put32(h + 24, s->ctx_image);
    jd_put32(h + 28, s->spu_id);
    jd_put32(h + 32, (uint32_t)s->thread);
    jd_put32(h + 36, s->resident_n);
    jd_put64(h + 40, (uint64_t)(uintptr_t)s->ctx);
    for (i = 0; i < 4; i++) jd_put32(h + 48 + 4 * i, s->entry_r3[i]);
    for (i = 0; i < 4; i++) jd_put32(h + 64 + 4 * i, s->entry_r4[i]);
    jd_put64(h + 80, s->t_entry);
    jd_put64(h + 88, t_close);
    jd_put64(h + 96, s->t_exit);
    jd_put32(h + 104, s->exits ? s->exit_pc : 0xFFFFFFFFu);
    jd_put32(h + 108, (uint32_t)reason);
    for (i = 0; i < 4; i++) {
        jd_put32(h + 112 + 12 * i, s->resident[i][0]);
        jd_put32(h + 116 + 12 * i, s->resident[i][1]);
        jd_put32(h + 120 + 12 * i, s->resident[i][2]);
    }
    memset(x, 0, sizeof(x));
    jd_put32(x, s->exits);
    jd_put32(x + 4, s->exit_pc);
    jd_put32(x + 8, s->exit_r0);
    jd_put32(x + 12, s->exit_r1);
    jd_put32(x + 16, s->exit_r3);
    jd_put32(x + 20, s->exit_r4);
    jd_put64(x + 24, s->t_exit);
    memset(e, 0, sizeof(e));
    jd_put32(e, s->dma_n);
    jd_put32(e + 4, s->dma_no_data);
    jd_put32(e + 8, s->dma_lost);
    jd_put32(e + 12, s->exits);
    jd_put32(e + 16, buffer_records);
    jd_put32(e + 20, (uint32_t)reason);
    jd_put64(e + 24, s->get_bytes);
    jd_put64(e + 32, s->put_bytes);
    jd_put32(e + 40, s->get_n);
    jd_put32(e + 44, s->put_n);
    jd_put64(e + 48, t_close - s->t_entry);
    snprintf(path, sizeof(path), "%s/job_%06u.bin", s_jd_cfg.dir, s->seq);
    f = fopen(path, "wb");
    ok = f != NULL && jd_write(f, h, sizeof(h)) &&
         jd_write_block(f, JD_REC_ENTRY_LS, 0, NULL, 0, s->entry_ls) &&
         jd_write(f, s->buf, s->buf_size);
    if (ok && s->exits)
        ok = jd_write_block(f, JD_REC_EXIT_LS, number++, x, JD_EXIT_HEAD, s->exit_ls);
    if (ok)
        ok = jd_write_block(f, JD_REC_END, number++, e, JD_END_HEAD, NULL);
    if (f && fclose(f)) ok = 0;
    s->active = 0;
    s->in_span = 0;
    s->buf_size = 0;
    if (!ok) {
        remove(path);
        fprintf(stderr, "[job-dump] cannot write %s\n", path);
        jd_turn_off();
        return;
    }
    s_jd_written++;
    fprintf(stderr, "[job-dump] job %u: image=%u spu=%u dma=%u get=%u (%llu bytes) "
                    "put=%u (%llu bytes) exits=%u job_ms=%.3f close=%d\n",
            s->seq, s->ctx_image, s->spu_id, s->dma_n, s->get_n,
            (unsigned long long)s->get_bytes, s->put_n,
            (unsigned long long)s->put_bytes, s->exits,
            (double)(t_close - s->t_entry) / 1e6, reason);
    fflush(stderr);
}

void spu_job_dump_probe(const struct spu_context* ctx, uint32_t pc)
{
    jd_slot* s;
    int span;
    jd_resolve();
    if (g_spu_job_dump != 1 || !ctx || !ctx->ls) return;
    jd_lock_take();
    span = jd_span(ctx, pc);
    s = jd_slot_find(ctx);
    if (span >= 0 && pc == s_jd_cfg.entry) {
        /* A second admission test of one launch is the same entry. */
        if (s && s->active && s->in_span && !s->records) {
            jd_lock_give();
            return;
        }
        if (s && s->active) jd_write_job(s, JD_CLOSE_NEXT_ENTRY);
        if (!s) {
            s = jd_slot_claim(ctx);
            if (!s) { jd_lock_give(); return; }
        }
        jd_start(s, ctx);
    } else if (s && s->active) {
        if (span >= 0) s->in_span = 1;
        else if (pc == SPURS_PM_SELECT_WORKLOAD_LS)
            jd_write_job(s, JD_CLOSE_SELECT_WORKLOAD);
        else if (s->in_span) jd_exit_point(s, ctx, pc);
    } else if (pc == s_jd_cfg.entry) {
        static int shown;
        if (shown++ < 4)
            fprintf(stderr, "[job-dump] entry 0x%05X reached, but image %u has no "
                            "resident span on this context; no dump\n",
                    pc, s_jd_cfg.image);
    }
    jd_lock_give();
}

void spu_job_dump_dma(const struct spu_context* ctx, uint32_t lsa, uint32_t ea,
                      uint32_t size, uint32_t cmd, int is_get, const uint8_t* bytes)
{
    jd_slot* s;
    jd_resolve();
    if (g_spu_job_dump != 1 || !size || !bytes) return;
    jd_lock_take();
    s = jd_slot_find(ctx);
    if (s && s->active)
        jd_append_dma(s, ctx, lsa, ea, size, cmd, is_get, bytes,
                      jd_span(ctx, (uint32_t)ctx->pc & SPU_LS_MASK) >= 0);
    jd_lock_give();
}

void spu_job_dump_flush(void)
{
    unsigned i;
    if (g_spu_job_dump != 1) return;
    jd_lock_take();
    for (i = 0; i < JD_SLOTS; i++)
        if (s_jd_slots[i].active) jd_write_job(&s_jd_slots[i], JD_CLOSE_PROCESS_EXIT);
    jd_lock_give();
    fprintf(stderr, "[job-dump] exit: jobs started %u, files written %u\n",
            s_jd_started, s_jd_written);
    fflush(stderr);
}
