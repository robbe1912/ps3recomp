/* Authored client and renderer model for the fast CPU surface read. The D3D12
 * GPU model and the scenario helpers are the ones of the surface prefetch
 * fixture; the checker inserts both at their marks. The production coherent
 * path, surface prefetch, request fast path and view publishers are injected
 * unchanged.
 *
 * A client read is served on the calling thread only when the bytes of every
 * surface it touches are current. Every other read must take the queued
 * path. */
#include <stddef.h>
/* Threads without <windows.h>: its typedefs conflict with the GPU model. */
#ifdef _WIN32
__declspec(dllimport) void *CreateThread(void *attributes, size_t stack_size,
                                         unsigned long (*start)(void *), void *parameter,
                                         unsigned long flags, unsigned long *identifier);
__declspec(dllimport) void Sleep(unsigned long milliseconds);
#else
#include <pthread.h>
#include <sched.h>
#endif
/* Pack seam: the client read of the checker arrives while the renderer packs. */
#define rsx_surface_readback_rows fixture_pack_rows
/* PREFETCH_MODEL */
#undef rsx_surface_readback_rows
#include "ps3emu/guest_read_sync.h"

static int s_cpu_surface_recording;
typedef struct {
  u32 cpu_address[4], cpu_pitch[4];
  unsigned rt_fmt;
} D3D12DrawRecord;
static int rsx_surface_readback_rows(void *guest, size_t guest_bytes, unsigned guest_pitch,
                                     const void *gpu, size_t gpu_bytes, unsigned gpu_pitch,
                                     unsigned width, unsigned height, unsigned format);
/* PRODUCTION_REGION */
/* PRODUCTION_TAIL */
/* PREFETCH_HELPERS */

/* The queue state of the request path is not part of this model. Name it, so
 * that a build with -Werror accepts the production block. */
static void name_queue_state(void) {
  (void)s_cpu_surface_lock;
  (void)s_cpu_surface_head;
  (void)s_cpu_surface_tail;
  (void)s_cpu_surface_live;
}

/* ---- The client read that arrives while the renderer packs ---- */
#define CLIENT 7u
#define OLD 0xA5u /* guest bytes before any copy */
static u32 hook_address, hook_bytes;
static unsigned char hook_old, hook_new;
static int pack_hook; /* 1: before the first row; 2: after the first row */
/* kind 1: the client read arrives before the first row; 2: after the first
 * row. The row of the read still holds the bytes before the copy. */
static void arm_hook(int kind, unsigned char value) {
  pack_hook = kind;
  hook_address = 0x1020;
  hook_bytes = 4;
  hook_old = vm_base[hook_address];
  hook_new = value;
}

static void fail(const char *message) {
  fprintf(stderr, "FAIL: %s\n", message);
  exit(1);
}
static void client_read_while_packing(void) {
  REQUIRE(vm_base[hook_address] == hook_old,
          "the client read arrives before the renderer packs its row");
  if (cpu_surface_fast_request(hook_address, hook_bytes, CLIENT))
    fail("a client read was served while the renderer packed a copy");
}
static int rsx_surface_readback_rows(void *guest, size_t guest_bytes, unsigned guest_pitch,
                                     const void *gpu, size_t gpu_bytes, unsigned gpu_pitch,
                                     unsigned width, unsigned height, unsigned format) {
  unsigned first = height;
  if (pack_hook == 1) {
    pack_hook = 0;
    client_read_while_packing();
  } else if (pack_hook == 2)
    first = 1;
  int result = fixture_pack_rows(guest, guest_bytes, guest_pitch, gpu, gpu_bytes, gpu_pitch,
                                 width, first, format);
  if (result || first == height)
    return result;
  pack_hook = 0;
  /* Row 0 holds the new generation. The row of the client read is still old. */
  REQUIRE(vm_base[hook_address - guest_pitch] == hook_new, "the first row is packed");
  client_read_while_packing();
  return fixture_pack_rows((unsigned char *)guest + guest_pitch, guest_bytes - guest_pitch,
                           guest_pitch, (const unsigned char *)gpu + gpu_pitch,
                           gpu_bytes - gpu_pitch, gpu_pitch, width, height - first, format);
}

/* ---- Roles ---- */
/* The renderer binds a 4x2 surface (pitch 32, 8 bytes a pixel) at address. */
static OffRT *bind_surface(int slot, u32 off, u32 address) {
  OffRT *r = surface(slot, off, address);
  D3D12DrawRecord draw;
  memset(&draw, 0, sizeof draw);
  draw.cpu_address[0] = address;
  draw.cpu_pitch[0] = 32;
  draw.rt_fmt = 11;
  cpu_surface_bind(slot, &draw, 0);
  return r;
}
/* A recorded draw wrote r (mrt: as its second target). The batch is submitted. */
static void write(OffRT *r, unsigned char value, int mrt) {
  Op op = {OP_WRITE, NULL, r->res, 0, value, NULL, 0};
  gpu_push(op);
  int secondary[3] = {-1, -1, -1};
  if (mrt) {
    secondary[1] = (int)(r - s_d3d.off_rt);
    cpu_surface_written(-1, secondary);
  } else
    cpu_surface_written((int)(r - s_d3d.off_rt), secondary);
  cpu_surface_prefetch_refresh();
}
/* The renderer serves a queued read. */
static void serve(u32 address, u32 bytes) { cpu_surface_read(address, bytes); }
static u64 view_count(void) {
  return cpu_surface_view_load(&s_cpu_surface_views) >> CPU_SURFACE_VIEW_COUNT;
}
static const CpuSurfaceView *view_table(void) {
  return (const CpuSurfaceView *)(uintptr_t)(cpu_surface_view_load(&s_cpu_surface_views) &
                                             CPU_SURFACE_VIEW_ADDRESS);
}
/* The number of views that serve the read, by the table a client reads. */
static u64 ready_views(u32 address, u32 bytes) {
  u64 hits = 0;
  const CpuSurfaceView *v = view_table();
  for (u64 i = view_count(); i; i--, v++)
    if (cpu_surface_view_probe(v, address, bytes) == CPU_SURFACE_VIEW_READY)
      hits++;
  return hits;
}
/* A client read that must be served on the calling thread. */
static void client_served(u32 address, u32 bytes, const char *what) {
  if (!cpu_surface_fast_request(address, bytes, CLIENT)) {
    fprintf(stderr, "FAIL: a client read was not served: %s\n", what);
    exit(1);
  }
  if (s_cpu_surface_profile)
    printf("FAST address=%08X bytes=%u hits=%llu\n", address, bytes,
           (unsigned long long)ready_views(address, bytes));
}
/* A client read that must take the queued path. */
static void client_queued(u32 address, u32 bytes, const char *what) {
  if (cpu_surface_fast_request(address, bytes, CLIENT)) {
    fprintf(stderr, "FAIL: a client read was served: %s\n", what);
    exit(1);
  }
}
#define EXPECT_STATE(index, address, bytes, state)                                              \
  do {                                                                                          \
    const CpuSurfaceView *view_ = cpu_surface_view_entry(index);                                 \
    REQUIRE(view_ && cpu_surface_view_probe(view_, address, bytes) == (state),                   \
            "view " #index " is not " #state);                                                   \
  } while (0)

/* ---- Two threads: the renderer changes a view, a client reads it ---- */
#ifdef _WIN32
static unsigned long thread_entry(void *body) {
  (*(void (*)(void))body)();
  return 0;
}
static int start_thread(void (*body)(void)) {
  /* The thread handle ends with the process. */
  return CreateThread(NULL, 0, thread_entry, body, 0, NULL) != NULL;
}
static void yield_cpu(void) { Sleep(0); }
#else
static void *thread_entry(void *body) {
  (*(void (*)(void))body)();
  return NULL;
}
static int start_thread(void (*body)(void)) {
  pthread_t thread;
  if (pthread_create(&thread, NULL, thread_entry, body))
    return 0;
  pthread_detach(thread);
  return 1;
}
static void yield_cpu(void) { sched_yield(); }
#endif

#define STRESS_ITERATIONS 4000000ull
static atomic_int stress_writer_done, stress_reader_done;
static atomic_ullong stress_probes[4];
/* The view alternates between a pending range that holds the probe address and
 * a ready range away from it. A torn view can report the address ready. */
static void stress_writer(void) {
  OffRT *r = &s_d3d.off_rt[0];
  for (u64 i = 0; i < STRESS_ITERATIONS; i++) {
    r->cpu_address = 0x1000;
    r->gpu_generation = 1;
    r->cpu_generation = 0;
    cpu_surface_view_update(r);
    r->cpu_address = 0x3000;
    r->gpu_generation = 5;
    r->cpu_generation = 5;
    cpu_surface_view_update(r);
  }
  atomic_store_explicit(&stress_writer_done, 1, memory_order_release);
}
static void stress_reader(void) {
  while (!atomic_load_explicit(&stress_writer_done, memory_order_acquire)) {
    CpuSurfaceView *v = cpu_surface_view_entry(0);
    if (!v)
      continue;
    int state = cpu_surface_view_probe(v, 0x1020, 4);
    stress_probes[state]++;
    if (cpu_surface_fast_request(0x1020, 4, CLIENT))
      fail("a client read was served while the renderer changed the view");
  }
  atomic_store_explicit(&stress_reader_done, 1, memory_order_release);
}
static void wait_for(atomic_int *flag) {
  while (!atomic_load_explicit(flag, memory_order_acquire))
    yield_cpu();
}
static void stress(void) {
  REQUIRE(start_thread(stress_writer), "renderer thread");
  REQUIRE(start_thread(stress_reader), "client thread");
  wait_for(&stress_writer_done);
  wait_for(&stress_reader_done);
  u64 probes = 0;
  for (int i = 0; i < 4; i++)
    probes += atomic_load(&stress_probes[i]);
  REQUIRE(probes >= 1000, "the client read ran beside the renderer");
  printf("STRESS iterations=%llu probes=%llu pending=%llu clear=%llu changing=%llu\n",
         (unsigned long long)STRESS_ITERATIONS, (unsigned long long)probes,
         (unsigned long long)atomic_load(&stress_probes[CPU_SURFACE_VIEW_PENDING]),
         (unsigned long long)atomic_load(&stress_probes[CPU_SURFACE_VIEW_CLEAR]),
         (unsigned long long)atomic_load(&stress_probes[CPU_SURFACE_VIEW_CHANGING]));
}
int main(void) {
#ifdef _WIN32
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  static uint8_t memory[0x10000];
  vm_base = memory;
  memset(memory, OLD, sizeof memory);
  s_cpu_surface_fast = 1;
  boot("");
  const u32 inside = 0x1020; /* row 1, column 0 of the surface at 0x1000 */

  /* A. Serving rules. */
  REQUIRE(view_count() == 0, "no surface: no view");
  client_queued(inside, 4, "no surface is bound");
  OffRT *a = bind_surface(0, 0x1F00, 0x1000);
  REQUIRE(vm_read_sync_page(1) == 1, "the bind marks the guest pages");
  EXPECT_STATE(0, inside, 4, CPU_SURFACE_VIEW_READY);
  client_queued(0x0FFC, 4, "the bytes below a surface");
  client_queued(0x1040, 4, "the bytes above a surface");
  s_cpu_surface_profile = 1;
  s_cpu_surface_profile_frequency = 1000;
  client_served(inside, 4, "a surface without a recorded write");
  s_cpu_surface_profile = 0;
  cpu_surface_view_recorded(1);
  client_queued(inside, 4, "a pending batch can write the ready surface");
  cpu_surface_view_recorded(0);
  s_cpu_surface_profile = 1;
  client_served(0x1000, 64, "the whole range of a surface");
  s_cpu_surface_profile = 0;

  /* A recorded write makes the client wait for the renderer's copy. */
  write(a, 0x11, 0);
  EXPECT_STATE(0, inside, 4, CPU_SURFACE_VIEW_PENDING);
  REQUIRE(vm_base[inside] == OLD, "guest memory still holds the old bytes");
  client_queued(inside, 4, "a superseded generation");
  client_queued(0x1000, 64, "a superseded generation, whole range");
  client_queued(0x0FFC, 4, "the bytes below a pending surface");
  arm_hook(1, 0x11);
  serve(inside, 4);
  REQUIRE(!pack_hook && vm_base[0x1000] == 0x11 && vm_base[0x103F] == 0x11,
          "the renderer copied generation 1");
  EXPECT_STATE(0, inside, 4, CPU_SURFACE_VIEW_READY);
  /* A client read during the pack of the next generation must queue too. */
  write(a, 0x22, 0);
  EXPECT_STATE(0, inside, 4, CPU_SURFACE_VIEW_PENDING);
  arm_hook(2, 0x22);
  serve(inside, 4);
  REQUIRE(!pack_hook && vm_base[0x1020] == 0x22 && vm_base[0x103F] == 0x22,
          "the renderer copied generation 2");
  client_served(inside, 4, "a copied generation");

  /* Two surfaces side by side: every surface the read touches must be ready. */
  OffRT *b = bind_surface(1, 0x2F00, 0x1040);
  EXPECT_STATE(1, 0x1040, 4, CPU_SURFACE_VIEW_READY);
  s_cpu_surface_profile = 1;
  client_served(0x103C, 8, "a read across two ready surfaces");
  s_cpu_surface_profile = 0;
  write(b, 0x33, 1);
  EXPECT_STATE(1, 0x1040, 4, CPU_SURFACE_VIEW_PENDING);
  client_queued(0x103C, 8, "a read that touches a pending surface");
  client_queued(0x1040, 4, "a pending surface");
  client_queued(0x103E, 4, "a read that touches a pending surface");
  client_served(0x103C, 4, "a read that ends where a pending surface begins");
  client_served(0x1000, 64, "the ready surface beside a pending one");
  serve(0x1040, 4);
  REQUIRE(vm_base[0x1040] == 0x33, "the renderer copied the second surface");
  write(a, 0x44, 0);
  EXPECT_STATE(0, inside, 4, CPU_SURFACE_VIEW_PENDING);
  client_served(0x1040, 4, "a read that begins where a pending surface ends");
  client_queued(0x103C, 4, "a pending surface");
  client_queued(0x103E, 4, "a read that touches a pending surface");
  serve(0x1000, 4);
  REQUIRE(vm_base[inside] == 0x44, "the renderer copied generation 3");

  /* A retired surface keeps its final bytes and loses its view. */
  cpu_surface_retire(b);
  EXPECT_STATE(1, 0x1040, 4, CPU_SURFACE_VIEW_CLEAR);
  client_queued(0x1040, 4, "a retired surface");
  REQUIRE(vm_base[0x1040] == 0x33, "a retired surface keeps its last copy");
  /* RSX_CPU_SURFACE_FAST=0 keeps every read queued. */
  s_cpu_surface_fast = 0;
  client_queued(0x103C, 4, "the fast path is off");
  s_cpu_surface_fast = 1;
  client_served(0x103C, 4, "the fast path is on");
  /* A view that the renderer is changing is never used. */
  u64 sequence = cpu_surface_view_load(&cpu_surface_view_entry(0)->sequence);
  cpu_surface_view_store(&cpu_surface_view_entry(0)->sequence, sequence + 1);
  EXPECT_STATE(0, inside, 4, CPU_SURFACE_VIEW_CHANGING);
  client_queued(inside, 4, "the renderer changes the view");
  cpu_surface_view_store(&cpu_surface_view_entry(0)->sequence, sequence);
  client_served(inside, 4, "the renderer finished the view");

  /* B. A served read asks the prefetch for the next generation. */
  boot("1F00");
  cpu_surface_retire(a);
  a = bind_surface(0, 0x1F00, 0x1000);
  client_served(inside, 4, "a listed surface without a recorded write");
  write(a, 0x11, 0);
  REQUIRE(prefetch_copies() == 1 && entry0()->generation == 1,
          "the served read asked for a prefetch of generation 1");
  gpu_run();
  EXPECT_STATE(0, inside, 4, CPU_SURFACE_VIEW_PENDING);
  client_queued(inside, 4, "a prefetched generation not packed yet");
  arm_hook(1, 0x11);
  serve(inside, 4);
  REQUIRE(!pack_hook && entry0()->used == 1 && coherent_copies() == 0,
          "the renderer served generation 1 from the prefetch");
  EXPECT_STATE(0, inside, 4, CPU_SURFACE_VIEW_READY);
  client_served(inside, 4, "a generation served from the prefetch");
  write(a, 0x22, 0);
  REQUIRE(prefetch_copies() == 2, "the served read asked for a prefetch of generation 2");
  gpu_run();
  arm_hook(2, 0x22);
  serve(inside, 4);
  REQUIRE(!pack_hook && entry0()->used == 2, "the renderer served generation 2 from the prefetch");
  /* The refresh moves the note of a served read into the prefetch request. */
  client_served(inside, 4, "a surface before an idle refresh");
  REQUIRE(cpu_surface_view_load(&cpu_surface_view_entry(0)->noted) == 1, "a served read is noted");
  cpu_surface_prefetch_refresh();
  REQUIRE(cpu_surface_view_load(&cpu_surface_view_entry(0)->noted) == 0 && entry0()->wanted == 1 &&
              prefetch_copies() == 2,
          "the refresh consumes the note; no new generation, no copy");
  /* The request asks for one generation. No read since: no copy for the next. */
  write(a, 0x33, 0);
  REQUIRE(prefetch_copies() == 3, "the request asked for a prefetch of generation 3");
  gpu_run();
  write(a, 0x44, 0);
  REQUIRE(prefetch_copies() == 3, "a read is noted once");
  serve(inside, 4);
  REQUIRE(coherent_copies() == 1 && vm_base[inside] == 0x44,
          "generation 4 took a coherent copy");
  /* A retired surface's noted read does not reach the next surface. */
  client_served(inside, 4, "a listed surface before it retires");
  cpu_surface_retire(a);
  REQUIRE(cpu_surface_view_load(&cpu_surface_view_entry(0)->noted) == 0,
          "retire consumes the noted read");
  EXPECT_STATE(0, inside, 4, CPU_SURFACE_VIEW_CLEAR);
  client_queued(inside, 4, "a retired listed surface");
  a = bind_surface(0, 0x1F00, 0x1000);
  write(a, 0x55, 0);
  REQUIRE(prefetch_copies() == 3, "the noted read did not reach the next surface");
  gpu_run();
  serve(inside, 4);

  /* C. The table grows with the surface pool. */
  client_served(inside, 4, "a listed surface before the pool grows");
  CpuSurfaceView *replaced = cpu_surface_view_entry(0);
  REQUIRE(view_count() == 16 && replaced, "the first table has 16 entries");
  REQUIRE(cpu_surface_view_slot(16) == cpu_surface_view_entry(16) && view_count() == 32,
          "slot 16 grew the table to 32 entries");
  REQUIRE(cpu_surface_view_load(&replaced->sequence) & 1, "the replaced table never serves again");
  EXPECT_STATE(0, inside, 4, CPU_SURFACE_VIEW_READY);
  REQUIRE(cpu_surface_view_load(&cpu_surface_view_entry(0)->noted) == 1,
          "the note survived the growth");
  client_served(inside, 4, "a surface after its table grew");
  REQUIRE(cpu_surface_view_slot(100) == cpu_surface_view_entry(100) && view_count() == 128,
          "slot 100 grew the table to 128 entries");
  client_served(inside, 4, "a surface after a second growth");
  REQUIRE(!cpu_surface_view_slot(70000) && view_count() == 128, "no view beyond the entry limit");

  /* D. The invariant on two threads. */
  stress();
  puts("PASS: fast CPU surface read serves current bytes only, queues every other read, notes the prefetch");
  return 0;
}
