/* Authored header and simultaneous-first-call contracts. No guest input. */
#include "../spu_jobmod_identity.h"
#include <stdatomic.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#define WORKERS 8
#define CHECK(value) do { if (!(value)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

static atomic_int ready, start, done, release_workers;
typedef struct worker_result {
    const int* mode;
    const unsigned* logs;
} worker_result;
static worker_result results[WORKERS];

static void yield_worker(void)
{
#ifdef _WIN32
    Sleep(0);
#else
    sched_yield();
#endif
}

static void exercise_worker(worker_result* result)
{
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&start)) yield_worker();
    CHECK(spu_jobmod_header_fnv_enabled() == 1);
    CHECK(spu_jobmod_header_fnv_enabled() == 1);
    for (unsigned i = 0; i < 32; ++i)
        CHECK(spu_jobmod_header_log_enabled() == (i < 16));
    result->mode = &s_spu_jobmod_header_mode;
    result->logs = &s_spu_jobmod_header_logs;
    atomic_fetch_add(&done, 1);
    while (!atomic_load(&release_workers)) yield_worker();
}

#ifdef _WIN32
static DWORD WINAPI worker(void* argument) { exercise_worker(argument); return 0; }
#else
static void* worker(void* argument) { exercise_worker(argument); return NULL; }
#endif

static void put_be32(uint8_t* target, uint32_t value)
{
    target[0] = (uint8_t)(value >> 24);
    target[1] = (uint8_t)(value >> 16);
    target[2] = (uint8_t)(value >> 8);
    target[3] = (uint8_t)value;
}

int main(int argc, char** argv)
{
    if (argc > 1 && !strcmp(argv[1], "env")) {
        printf("%d\n", spu_jobmod_header_fnv_enabled());
        return 0;
    }
    uint8_t header[0x40] = {0};
    put_be32(header + 0x14, 0x1020);
    put_be32(header + 0x20, 0xC0DEC0DE);
    put_be32(header + 0x28, 0x4000);
    CHECK(stage_header_span(header, 0x4000, sizeof header) == 0x1000);
    CHECK(stage_header_span(header, 0x8000, sizeof header) == 0);
    CHECK(stage_header_span(header, 0x4000, 0x2f) == 0);
    put_be32(header + 0x14, 0x800);
    CHECK(stage_header_span(header, 0x4000, sizeof header) == 0);
    put_be32(header + 0x14, 0x40000);
    CHECK(stage_header_span(header, 0x4000, sizeof header) == 0);
#ifdef _WIN32
    CHECK(_putenv_s("SPU_JOBMOD_HEADER_FNV", "1") == 0);
    HANDLE threads[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i) {
        threads[i] = CreateThread(NULL, 0, worker, &results[i], 0, NULL);
        CHECK(threads[i]);
    }
#else
    CHECK(setenv("SPU_JOBMOD_HEADER_FNV", "1", 1) == 0);
    pthread_t threads[WORKERS];
    for (unsigned i = 0; i < WORKERS; ++i)
        CHECK(pthread_create(&threads[i], NULL, worker, &results[i]) == 0);
#endif
    while (atomic_load(&ready) != WORKERS) yield_worker();
    atomic_store(&start, 1);
    while (atomic_load(&done) != WORKERS) yield_worker();
    for (unsigned i = 0; i < WORKERS; ++i) {
        for (unsigned j = 0; j < i; ++j) {
            CHECK(results[i].mode != results[j].mode);
            CHECK(results[i].logs != results[j].logs);
        }
    }
    atomic_store(&release_workers, 1);
    for (unsigned i = 0; i < WORKERS; ++i) {
#ifdef _WIN32
        CHECK(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0);
        CHECK(CloseHandle(threads[i]));
#else
        CHECK(pthread_join(threads[i], NULL) == 0);
#endif
    }
    puts("PASS header bounds and eight simultaneous first callers with private selector/log state");
    return 0;
}
