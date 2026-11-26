/* Internal per-context MFC registry; included once by spu_channels.c.
 * Physical SPU count does not bound retained software contexts. Overflow
 * contexts own private engines until release, just like the inline owners. */
#define SPU_MAX_CONTEXTS 8

typedef struct {
    PVOID volatile ctx;
    mfc_engine   mfc;
} spu_mfc_slot;

static spu_mfc_slot s_mfc_slots[SPU_MAX_CONTEXTS];
static SRWLOCK      s_mfc_claim_lock = SRWLOCK_INIT;

/* Physical SPUs do not bound retained software contexts: virtual policies
 * and workloads can each own an engine while inactive. Overflow owners keep
 * private state until their context is released. All list access is locked. */
typedef struct spu_mfc_overflow {
    spu_context* ctx;
    mfc_engine mfc;
    struct spu_mfc_overflow* next;
} spu_mfc_overflow;
static spu_mfc_overflow* s_mfc_overflow;
/* Lookup/use may not overlap release of that same context: release happens
 * after its SPU stops. A different host can release it, so TLS alone is not
 * sufficient. The acquire-read generation rejects cached pointers after any
 * release, including a context later allocated at the same address. */
static volatile LONG64 s_mfc_generation = 1;
static SPU_THREAD_LOCAL struct {
    spu_context* ctx;
    mfc_engine* engine;
    LONG64 generation;
} s_mfc_cached;

static mfc_engine* mfc_remember(spu_context* ctx, mfc_engine* engine,
                                LONG64 generation)
{
    s_mfc_cached.ctx = ctx;
    s_mfc_cached.engine = engine;
    s_mfc_cached.generation = generation;
    return engine;
}

static mfc_engine* mfc_for(spu_context* ctx)
{
    LONG64 generation = ReadAcquire64(&s_mfc_generation);
    if (s_mfc_cached.ctx == ctx && s_mfc_cached.generation == generation)
        return s_mfc_cached.engine;
    for (int i = 0; i < SPU_MAX_CONTEXTS; i++)
        if (ReadPointerAcquire(&s_mfc_slots[i].ctx) == ctx)
            return mfc_remember(ctx, &s_mfc_slots[i].mfc, generation);

    /* No slot yet: take one. The engine is initialized before the slot is
     * published, so it is never visible to anyone in a half-reset state. */
    mfc_engine* e = NULL;
    AcquireSRWLockExclusive(&s_mfc_claim_lock);
    /* Recheck publication after acquiring the lock. An existing overflow
     * owner must stay on its engine even when an inline slot becomes free. */
    for (int i = 0; i < SPU_MAX_CONTEXTS && !e; i++)
        if (ReadPointerAcquire(&s_mfc_slots[i].ctx) == ctx) e = &s_mfc_slots[i].mfc;
    for (spu_mfc_overflow* n = s_mfc_overflow; n && !e; n = n->next)
        if (n->ctx == ctx) e = &n->mfc;
    for (int i = 0; i < SPU_MAX_CONTEXTS && !e; i++) {
        if (ReadPointerAcquire(&s_mfc_slots[i].ctx) != NULL) continue;
        mfc_engine_init(&s_mfc_slots[i].mfc);
        WritePointerRelease(&s_mfc_slots[i].ctx, ctx);
        e = &s_mfc_slots[i].mfc;
    }
    if (!e) {
        spu_mfc_overflow* n = (spu_mfc_overflow*)calloc(1, sizeof(*n));
        if (!n) {
            fprintf(stderr, "[SPU] cannot allocate private MFC engine for context %p\n", (void*)ctx);
            fflush(stderr);
            abort();
        }
        mfc_engine_init(&n->mfc);
        n->ctx = ctx;
        n->next = s_mfc_overflow;
        s_mfc_overflow = n;
        e = &n->mfc;
    }
    ReleaseSRWLockExclusive(&s_mfc_claim_lock);
    return mfc_remember(ctx, e, generation);
}

/* Give the slot `ctx` holds back to the table, called when its context is done.
 * Slots used to be claimed and never released, so a title with more SPU
 * contexts over its life than there are slots ran the rest of them on the
 * shared fallback engine -- and by then it is not a fresh engine but whatever
 * tag and queue state the previous owners left in it.
 *
 * Release is allowed only after its SPU has stopped, so the engine is idle.
 * The next claimant re-initializes it. A context that never issued a
 * DMA holds no slot and this is a scan that finds nothing. */
void spu_mfc_release(spu_context* ctx)
{
    if (!ctx) return;
    AcquireSRWLockExclusive(&s_mfc_claim_lock);
    InterlockedIncrement64(&s_mfc_generation);
    for (int i = 0; i < SPU_MAX_CONTEXTS; i++) {
        if (ReadPointerAcquire(&s_mfc_slots[i].ctx) != ctx) continue;
        WritePointerRelease(&s_mfc_slots[i].ctx, NULL);
        break;
    }
    for (spu_mfc_overflow** link = &s_mfc_overflow; *link; link = &(*link)->next) {
        if ((*link)->ctx != ctx) continue;
        spu_mfc_overflow* n = *link;
        *link = n->next;
        free(n);
        break;
    }
    ReleaseSRWLockExclusive(&s_mfc_claim_lock);
}
