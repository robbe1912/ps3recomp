/* Authored client and renderer model for the snapshot fast path. The D3D12 GPU
 * model is the one of the surface prefetch fixture; the checker inserts it at
 * its mark. No GPU copy runs here: the renderer model packs the bytes itself,
 * as the production copy does, and only while the surface is not current.
 * The checker injects the production blocks unchanged: the request
 * declarations, the profile and view helpers, the view publishers, the surface
 * hooks, the snapshot fast path with the snapshot request, and the recorded
 * draws with the range check. The production guest_read_sync.c is the
 * transport.
 *
 * A snapshot request is served on the calling thread only when no recorded
 * draw targets a range and the one surface that meets a range is current and
 * holds the whole range. Every other request takes the boundary path: here the
 * queue, which the renderer serves when the client blocks. */
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
/* PREFETCH_MODEL */
#include "ps3emu/guest_read_sync.h"

/* ---- Threads, the queue and the process memory of the model ---- */
#define CLIENT 7u
#define RENDERER 2u
#define OLD 0xA5u /* output bytes before a request */
typedef struct {
  u32 cpu_address[4], cpu_pitch[4];
  unsigned rt_fmt, rt_h, rt_off, rt_mrt[3];
} D3D12DrawRecord;
static unsigned char fixture_memory[0x10000]; /* guest memory of the model */
static unsigned long fixture_thread = CLIENT;
static unsigned fixture_kicks;                /* renderer kicks of the client */
static unsigned fixture_reads;                /* process reads of the client */
static int fixture_unreadable_page = -1;      /* guest 4 KiB page the process cannot read */
static void (*fixture_hook)(void);            /* renderer action after a process read */
static unsigned long GetCurrentThreadId(void) { return fixture_thread; }
static LONG InterlockedCompareExchange(volatile LONG *p, LONG value, LONG expected) {
  __atomic_compare_exchange_n(p, &expected, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return expected;
}
static unsigned fixture_locks_held;
static void AcquireSRWLockExclusive(SRWLOCK *lock) { (void)lock; fixture_locks_held++; }
static void ReleaseSRWLockExclusive(SRWLOCK *lock) {
  (void)lock;
  REQUIRE(fixture_locks_held, "release of a lock that is not held");
  fixture_locks_held--;
}
static FakeEvent fixture_kick;
void *cellGcm_fifo_kick_event(void) { return &fixture_kick; }
static int SetEvent(HANDLE event) {
  FakeEvent *e = event;
  if (e == &fixture_kick)
    fixture_kicks++;
  e->armed = 1;
  return 1;
}
static void fail(const char *message) {
  fprintf(stderr, "FAIL: %s\n", message);
  exit(1);
}
static void *GetCurrentProcess(void) { return (void *)1; }
static int ReadProcessMemory(void *process, const void *address, void *destination,
                             size_t bytes, size_t *copied) {
  (void)process;
  const unsigned char *source = address;
  size_t offset = (size_t)(source - vm_base);
  *copied = 0;
  if (source < vm_base || offset > sizeof fixture_memory || bytes > sizeof fixture_memory - offset)
    return 0;
  if (fixture_unreadable_page >= 0 && fixture_unreadable_page >= (int)(offset >> 12) &&
      fixture_unreadable_page <= (int)((offset + bytes - 1) >> 12))
    return 0;
  memcpy(destination, source, bytes);
  *copied = bytes;
  fixture_reads++;
  REQUIRE(fixture_thread == CLIENT, "the fast path reads on the calling thread");
  if (fixture_hook) {
    void (*hook)(void) = fixture_hook;
    fixture_hook = NULL;
    hook();
  }
  return 1;
}
/* Production code that waits asks the renderer for its boundary. */
static DWORD fixture_queue_wait(HANDLE event, DWORD milliseconds);
#define WaitForSingleObject fixture_queue_wait
static unsigned fixture_scalar_reads;
/* PRODUCTION_DECLARATIONS */
/* The renderer service reads renderer state. A client thread that reads it fails. */
static int recording_for_owner(void) {
  if ((LONG)GetCurrentThreadId() != InterlockedCompareExchange(&s_cpu_surface_thread, 0, 0))
    fail("renderer state read by another thread");
  return s_cpu_surface_recording;
}
#define s_cpu_surface_recording recording_for_owner()
static void cpu_surface_read(u32 address, u32 bytes) {
  (void)bytes;
  if (fixture_locks_held)
    fail("surface copy under the request lock");
  fixture_scalar_reads++;
  (void)address;
}
/* PRODUCTION_HELPERS */
/* PRODUCTION_VIEWS */
/* PRODUCTION_HOOKS */
/* PRODUCTION_SNAPSHOT_FAST */
/* PRODUCTION_PROMPT */
#undef s_cpu_surface_recording
#undef WaitForSingleObject
/* The GPU command state and the device tables of the model are not used here.
 * Name them, so that a build with -Werror accepts the production declarations. */
static void name_unused_state(void) {
  (void)IID_ID3D12Resource;
  (void)IID_ID3D12CommandAllocator;
  (void)IID_ID3D12GraphicsCommandList;
  (void)IID_ID3D12Fence;
  (void)s_queue_vtbl;
  (void)s_dev_vtbl;
  (void)s_cpu_surface_alloc;
  (void)s_cpu_surface_list;
  (void)s_cpu_surface_fence;
  (void)s_cpu_surface_event;
  (void)s_cpu_surface_fence_value;
}

/* ---- The boundary path of the model ---- */
static unsigned fixture_boundaries; /* boundaries that answered a client */
/* What the renderer answers a queued request with. */
static int (*fixture_boundary)(const VmReadSnapshotRange *ranges, u32 count, void *staging, u32 output_bytes);
static int boundary_unavailable(const VmReadSnapshotRange *ranges, u32 count, void *staging, u32 output_bytes) {
  (void)ranges;
  (void)count;
  (void)staging;
  (void)output_bytes;
  return VM_READ_SNAPSHOT_UNAVAILABLE;
}
static int boundary_bytes(const VmReadSnapshotRange *ranges, u32 count, void *staging, u32 output_bytes) {
  (void)ranges;
  (void)count;
  memset(staging, 0xE7, output_bytes);
  return VM_READ_SNAPSHOT_OK;
}
/* The renderer at its FIFO boundary: it takes the whole queue and answers each request. */
static void fixture_boundary_service(void) {
  CpuSurfaceRequest *request = s_cpu_surface_head;
  s_cpu_surface_head = s_cpu_surface_tail = NULL;
  while (request) {
    CpuSurfaceRequest *next = request->next;
    request->profile.started = cpu_surface_tick();
    s_cpu_surface_profile_active = s_cpu_surface_profile ? &request->profile : NULL;
    request->snapshot_result = fixture_boundary(request->snapshot_ranges, request->snapshot_count,
                                                request->snapshot_staging, request->snapshot_output_bytes);
    request->status = 0;
    s_cpu_surface_profile_active = NULL;
    request->profile.finished = cpu_surface_tick();
    SetEvent(request->done);
    request = next;
  }
}
static DWORD fixture_queue_wait(HANDLE event, DWORD milliseconds) {
  (void)event;
  (void)milliseconds;
  fixture_boundaries++;
  fixture_thread = RENDERER;
  fixture_boundary_service();
  fixture_thread = CLIENT;
  return WAIT_OBJECT_0;
}

/* ---- Roles ---- */
/* The renderer binds a surface: height rows of pitch bytes at address. */
static OffRT *bind_surface(int slot, u32 address, u32 pitch, u32 height) {
  static ID3D12Resource owner; /* any owner: the view needs a live resource */
  OffRT *r = &s_d3d.off_rt[slot];
  r->res = &owner;
  r->w = pitch;
  r->h = height;
  D3D12DrawRecord draw;
  memset(&draw, 0, sizeof draw);
  draw.cpu_address[0] = address;
  draw.cpu_pitch[0] = pitch;
  draw.rt_fmt = 11;
  draw.rt_h = height;
  cpu_surface_bind(slot, &draw, 0);
  return r;
}
/* A recorded draw of the renderer wrote r: its generation is not current. */
static void write_surface(OffRT *r) {
  const int secondary[3] = {-1, -1, -1};
  cpu_surface_written((int)(r - s_d3d.off_rt), secondary);
}
/* The renderer copied the generation of r into guest memory. */
static void pack_surface(OffRT *r, unsigned char value) {
  REQUIRE(cpu_surface_view_probe(cpu_surface_view_entry((int)(r - s_d3d.off_rt)), r->cpu_address, 1) ==
              CPU_SURFACE_VIEW_PENDING,
          "the renderer packs only a surface that is not current");
  memset(vm_base + r->cpu_address, value, (size_t)r->cpu_pitch * r->h);
  r->cpu_generation = r->gpu_generation;
  cpu_surface_view_update(r);
}
/* The renderer recorded a draw whose colour target is address..address+pitch*height. */
static void record_draw(u32 address, u32 pitch, u32 height) { cpu_surface_pending_add(address, pitch, height); }
/* render_frame submitted the recorded draws. */
static void submit_batch(void) { cpu_surface_batch_submitted(); }
static u64 view_count(void) { return cpu_surface_view_load(&s_cpu_surface_views) >> CPU_SURFACE_VIEW_COUNT; }
/* The number of surfaces that hold a range of the request. */
static u64 holding_views(const VmReadSnapshotRange *ranges, u32 count) {
  u64 hits = 0;
  CpuSurfaceView *v = (CpuSurfaceView *)(uintptr_t)(cpu_surface_view_load(&s_cpu_surface_views) & CPU_SURFACE_VIEW_ADDRESS);
  for (u64 i = view_count(); i; i--, v++)
    for (u32 j = 0; j < count; j++)
      if (cpu_surface_view_probe(v, ranges[j].address, ranges[j].bytes) == CPU_SURFACE_VIEW_READY) {
        hits++;
        break;
      }
  return hits;
}

/* ---- Requests of the client ---- */
/* A request that the fast path must serve: the bytes of guest memory, no boundary, no kick. */
static void served(const VmReadSnapshotRange *ranges, u32 count, u32 output_bytes, const char *what) {
  unsigned char output[VM_READ_SNAPSHOT_MAX_BYTES], covered[VM_READ_SNAPSHOT_MAX_BYTES] = {0};
  memset(output, OLD, sizeof output);
  unsigned boundaries = fixture_boundaries, kicks = fixture_kicks;
  fixture_thread = CLIENT;
  if (vm_read_snapshot(ranges, count, output, output_bytes) != VM_READ_SNAPSHOT_OK) {
    fprintf(stderr, "FAIL: a snapshot request was not served: %s\n", what);
    exit(1);
  }
  if (fixture_boundaries != boundaries || fixture_kicks != kicks)
    fail("a served snapshot request reached the queue");
  for (u32 i = 0; i < count; i++) {
    if (memcmp(output + ranges[i].output_offset, vm_base + ranges[i].address, ranges[i].bytes))
      fail("a served snapshot request returned other bytes than guest memory");
    memset(covered + ranges[i].output_offset, 1, ranges[i].bytes);
  }
  for (u32 i = 0; i < output_bytes; i++)
    if (!covered[i] && output[i] != OLD)
      fail("a served snapshot request changed bytes outside its ranges");
  REQUIRE(!fixture_locks_held, "a served snapshot request left a lock held");
  if (s_cpu_surface_profile)
    printf("FAST address=%08X bytes=%u hits=%llu\n", ranges[0].address, output_bytes,
           (unsigned long long)holding_views(ranges, count));
}
/* A request that the fast path must decline: the boundary answers it with status. */
static void queued(const VmReadSnapshotRange *ranges, u32 count, u32 output_bytes, int status, const char *what) {
  unsigned char output[VM_READ_SNAPSHOT_MAX_BYTES], before[VM_READ_SNAPSHOT_MAX_BYTES];
  memset(output, OLD, sizeof output);
  memcpy(before, output, sizeof output);
  unsigned boundaries = fixture_boundaries;
  fixture_thread = CLIENT;
  int got = vm_read_snapshot(ranges, count, output, output_bytes);
  if (got != status) {
    fprintf(stderr, "FAIL: a snapshot request took another path (status %d): %s\n", got, what);
    exit(1);
  }
  if (fixture_boundaries != boundaries + 1)
    fail("a declined snapshot request did not reach the boundary");
  if (status == VM_READ_SNAPSHOT_OK) {
    for (u32 i = 0; i < output_bytes; i++)
      if (output[i] != 0xE7)
        fail("the boundary lost its bytes");
  } else if (memcmp(output, before, output_bytes))
    fail("a declined snapshot request changed the destination");
  REQUIRE(!fixture_locks_held, "a declined snapshot request left a lock held");
}
/* ---- The ranges of the scenarios: surface a is [0x1000, 0x1040), b is [0x1040, 0x1080) ---- */
static const VmReadSnapshotRange plain[] = {{0x2000, 1, 0}, {0x2001, 1, 1}};
static const VmReadSnapshotRange in_surface[] = {{0x1000, 1, 0}, {0x103F, 1, 1}};
static const VmReadSnapshotRange four[] = {{0x1000, 1, 0}, {0x1010, 1, 1}, {0x1020, 1, 2}, {0x1030, 1, 3}};
static const VmReadSnapshotRange mixed[] = {{0x1000, 1, 0}, {0x2000, 1, 1}};
static const VmReadSnapshotRange swapped[] = {{0x1000, 1, 1}, {0x2000, 1, 0}};
static const VmReadSnapshotRange crossing_end[] = {{0x103F, 2, 0}};
static const VmReadSnapshotRange crossing_begin[] = {{0x0FFF, 2, 0}};
static const VmReadSnapshotRange to_edge[] = {{0x103E, 2, 0}};
static const VmReadSnapshotRange below_edge[] = {{0x0FFE, 2, 0}};
static const VmReadSnapshotRange above_edge[] = {{0x1040, 2, 0}};
static const VmReadSnapshotRange across_two[] = {{0x103C, 8, 0}};
static const VmReadSnapshotRange in_two[] = {{0x1040, 1, 0}, {0x1030, 1, 1}};
static const VmReadSnapshotRange second[] = {{0x1044, 1, 0}};
static const VmReadSnapshotRange first_only[] = {{0x1038, 4, 0}};
static const VmReadSnapshotRange into_pending[] = {{0x103E, 4, 0}};
static const VmReadSnapshotRange claimed[] = {{0x1008, 1, 0}};
static const VmReadSnapshotRange draw_target[] = {{0x2000, 1, 0}};
static const VmReadSnapshotRange draw_last[] = {{0x203F, 1, 0}};
static const VmReadSnapshotRange draw_above[] = {{0x2040, 1, 0}};
static const VmReadSnapshotRange draw_below[] = {{0x1FFF, 1, 0}};
static const VmReadSnapshotRange draw_inside[] = {{0x2018, 1, 0}};
static const VmReadSnapshotRange unbound[] = {{0x2200, 1, 0}};
static const VmReadSnapshotRange unreadable[] = {{0x1000, 1, 0}, {0x3000, 1, 1}};
static const VmReadSnapshotRange beyond[] = {{0x1000, 1, 0}, {0x20000, 1, 1}};
#define UNAVAILABLE VM_READ_SNAPSHOT_UNAVAILABLE
#define BYTES(a, b) (a), (b)

/* ---- The renderer acts after the first read of one request ---- */
static OffRT *surface_a;
/* The renderer writes and packs a new generation of surface a. */
static void hook_new_generation(void) {
  write_surface(surface_a);
  pack_surface(surface_a, 0x66);
}
/* The renderer records a draw that targets the ranges. */
static void hook_targeting_draw(void) { record_draw(0x1000, 0x40, 1); }
/* The renderer records a draw elsewhere. */
static void hook_other_draw(void) { record_draw(0x6000, 0x40, 1); }
/* The surface pool grows: the renderer replaces the view table. */
static void hook_table_growth(void) {
  REQUIRE(cpu_surface_view_slot(16) == cpu_surface_view_entry(16) && view_count() == 32, "the view table grew");
}

/* ---- Two threads: the renderer changes a surface and the recorded draws ---- */
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
#define STRESS_LIMIT 20000000ull /* the renderer stops earlier when both paths ran enough */
#define STRESS_SERVED 2000ull
#define STRESS_DECLINED 1000ull
static atomic_int stress_renderer_done, stress_client_done;
static atomic_ullong stress_served, stress_declined, stress_iterations;
/* The view of slot 0 alternates between a generation that is not current and a
 * current one; the recorded draws alternate between one that targets the ranges
 * and none. The bytes of the surface change one by one while it is not current.
 * The current state stays for a while, so that a client is also served. */
static void stress_renderer(void) {
  OffRT *r = &s_d3d.off_rt[0];
  CpuSurfaceView *v = cpu_surface_view_entry(0);
  for (u64 i = 0; i < STRESS_LIMIT; i++) {
    write_surface(r);
    for (u32 b = 0; b < 64; b++)
      vm_base[0x1000 + b] = (unsigned char)(0x90 + (i & 1));
    r->cpu_generation = r->gpu_generation;
    cpu_surface_view_update(r);
    for (u32 spin = 0; spin < 256; spin++)
      (void)cpu_surface_view_load(&v->sequence);
    record_draw(0x1000, 0x40, 1);
    submit_batch();
    atomic_store_explicit(&stress_iterations, i + 1, memory_order_relaxed);
    if (!(i & 0xFFF) && atomic_load_explicit(&stress_served, memory_order_relaxed) >= STRESS_SERVED &&
        atomic_load_explicit(&stress_declined, memory_order_relaxed) >= STRESS_DECLINED)
      break;
  }
  atomic_store_explicit(&stress_renderer_done, 1, memory_order_release);
}
/* A client read of four bytes of the surface: the fast path only, no queue. */
static void stress_client(void) {
  while (!atomic_load_explicit(&stress_renderer_done, memory_order_acquire)) {
    unsigned char staging[4];
    memset(staging, OLD, sizeof staging);
    if (!cpu_surface_snapshot_fast(four, 4, staging, sizeof staging, CLIENT)) {
      atomic_fetch_add(&stress_declined, 1);
      continue;
    }
    atomic_fetch_add(&stress_served, 1);
    if (!(staging[0] == staging[1] && staging[1] == staging[2] && staging[2] == staging[3]))
      fail("a served snapshot holds the bytes of two states of the surface");
    if (staging[0] != 0x90 && staging[0] != 0x91)
      fail("a served snapshot holds bytes that the renderer did not write");
  }
  atomic_store_explicit(&stress_client_done, 1, memory_order_release);
}
static void stress(void) {
  REQUIRE(start_thread(stress_renderer), "renderer thread");
  REQUIRE(start_thread(stress_client), "client thread");
  while (!atomic_load_explicit(&stress_renderer_done, memory_order_acquire) ||
         !atomic_load_explicit(&stress_client_done, memory_order_acquire))
    yield_cpu();
  u64 served_count = atomic_load(&stress_served), declined = atomic_load(&stress_declined);
  printf("STRESS iterations=%llu served=%llu declined=%llu\n",
         (unsigned long long)atomic_load(&stress_iterations),
         (unsigned long long)served_count, (unsigned long long)declined);
  REQUIRE(served_count >= STRESS_SERVED && declined >= STRESS_DECLINED, "the client read beside the renderer");
}

int main(void) {
#ifdef _WIN32
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  vm_base = fixture_memory;
  memset(fixture_memory, OLD, sizeof fixture_memory);
  s_cpu_surface_thread = RENDERER;
  s_cpu_surface_live = 1;
  name_unused_state();
  vm_read_snapshot_set_handler(cpu_surface_snapshot_request, NULL);
  fixture_boundary = boundary_unavailable;

  /* A. The rules. */
  s_cpu_surface_snapshot_fast = 0;
  queued(plain, BYTES(2, 2), UNAVAILABLE, "the fast path is off");
  s_cpu_surface_snapshot_fast = 1;
  /* Three different bytes: a read of a wrong address cannot pass. */
  vm_base[0x2000] = 0x11;
  vm_base[0x2001] = 0x22;
  vm_base[0x2002] = 0x33;
  served(plain, BYTES(2, 2), "plain memory");
  /* The renderer thread keeps its refusal: it cannot wait for itself. */
  fixture_thread = RENDERER;
  {
    unsigned char output[2];
    memset(output, OLD, sizeof output);
    unsigned boundaries = fixture_boundaries, kicks = fixture_kicks;
    REQUIRE(vm_read_snapshot(plain, 2, output, 2) == VM_READ_SNAPSHOT_UNAVAILABLE && output[0] == OLD &&
                output[1] == OLD && fixture_boundaries == boundaries && fixture_kicks == kicks,
            "the renderer thread keeps its refusal");
  }
  fixture_thread = CLIENT;

  /* One surface. */
  surface_a = bind_surface(0, 0x1000, 16, 4);
  memset(vm_base + 0x1000, 0x31, 64);
  REQUIRE(vm_read_sync_page(1) == 1, "the bind marks the guest pages");
  served(in_surface, BYTES(2, 2), "a surface without a recorded write");
  served(to_edge, BYTES(1, 2), "a range up to the last byte of a surface");
  served(below_edge, BYTES(1, 2), "a range that ends where a surface begins");
  served(above_edge, BYTES(1, 2), "a range that begins where a surface ends");
  served(mixed, BYTES(2, 2), "a range in a surface beside a range in plain memory");
  served(swapped, BYTES(2, 2), "ranges whose output offsets differ from their order");
  queued(crossing_end, BYTES(1, 2), UNAVAILABLE, "a range that crosses the end of a surface");
  queued(crossing_begin, BYTES(1, 2), UNAVAILABLE, "a range that crosses the start of a surface");

  /* A recorded write makes the client wait for the renderer's copy. */
  write_surface(surface_a);
  queued(in_surface, BYTES(2, 2), UNAVAILABLE, "a superseded generation");
  queued(to_edge, BYTES(1, 2), UNAVAILABLE, "a superseded generation, whole range");
  served(above_edge, BYTES(1, 2), "the bytes above a pending surface");
  pack_surface(surface_a, 0x44);
  served(in_surface, BYTES(2, 2), "a copied generation");

  /* Two surfaces side by side. */
  OffRT *b = bind_surface(1, 0x1040, 16, 4);
  memset(vm_base + 0x1040, 0x32, 64);
  served(in_two, BYTES(2, 2), "ranges in two current surfaces");
  queued(across_two, BYTES(1, 8), UNAVAILABLE, "a range across two surfaces");
  write_surface(b);
  queued(second, BYTES(1, 1), UNAVAILABLE, "a second superseded generation");
  served(first_only, BYTES(1, 4), "a current surface beside a pending one");
  queued(into_pending, BYTES(1, 4), UNAVAILABLE, "a range that ends inside a pending surface");
  pack_surface(b, 0x55);
  /* Two surfaces claim one range: the ownership is ambiguous. */
  OffRT *c = bind_surface(2, 0x1000, 16, 4);
  queued(claimed, BYTES(1, 1), UNAVAILABLE, "a range that two surfaces claim");
  c->cpu_address = c->cpu_pitch = 0;
  c->gpu_generation = c->cpu_generation = 0;
  cpu_surface_view_retire(c);
  c->res = NULL;
  served(claimed, BYTES(1, 1), "the range after the second surface retired");

  /* Recorded draws of the current batch. */
  record_draw(0x2000, 0x40, 1);
  queued(draw_target, BYTES(1, 1), UNAVAILABLE, "a recorded draw targets the range");
  queued(draw_last, BYTES(1, 1), UNAVAILABLE, "the last byte of a recorded target");
  served(draw_above, BYTES(1, 1), "a range that begins where a recorded target ends");
  served(draw_below, BYTES(1, 1), "a range that ends where a recorded target begins");
  served(in_surface, BYTES(2, 2), "a surface that no recorded draw targets");
  record_draw(0x2010, 0x10, 1);
  queued(draw_inside, BYTES(1, 1), UNAVAILABLE, "a target inside a recorded target");
  record_draw(0x1F00, 0x100, 1);
  queued(draw_below, BYTES(1, 1), UNAVAILABLE, "a larger recorded target");
  submit_batch();
  served(draw_target, BYTES(1, 1), "after the batch was submitted");
  served(draw_below, BYTES(1, 1), "after the batch was submitted");
  record_draw(0x2100, 0x40, 0);
  queued(in_surface, BYTES(2, 2), UNAVAILABLE, "a target of unknown height");
  submit_batch();
  for (u32 i = 0; i <= CPU_SURFACE_PENDING_MAX; i++)
    record_draw(0x4000 + i * 0x40, 0x40, 1);
  queued(in_surface, BYTES(2, 2), UNAVAILABLE, "an overflowed list of recorded draws");
  submit_batch();
  for (u32 i = 0; i < CPU_SURFACE_PENDING_MAX; i++)
    record_draw(0x4000 + i * 0x40, 0x40, 1);
  served(in_surface, BYTES(2, 2), "a full list of recorded draws that does not target the range");
  submit_batch();
  record_draw(0, 0x40, 1);
  record_draw(0xffffffffu, 0x40, 1);
  record_draw(0x2200, 0, 1);
  served(unbound, BYTES(1, 1), "unbound targets are not recorded draws");
  submit_batch();

  /* Bytes that the process cannot read. */
  unsigned reads = fixture_reads;
  fixture_unreadable_page = 3;
  queued(unreadable, BYTES(2, 2), UNAVAILABLE, "a range in a page the process cannot read");
  REQUIRE(fixture_reads == reads + 1, "the client read the first range, then declined");
  fixture_unreadable_page = -1;
  queued(beyond, BYTES(2, 2), UNAVAILABLE, "a range beyond the guest memory of the model");
  served(mixed, BYTES(2, 2), "the same ranges after the page is readable");

  /* The renderer changes a view or the recorded draws. */
  CpuSurfaceView *view = cpu_surface_view_entry(0);
  u64 sequence = cpu_surface_view_load(&view->sequence);
  cpu_surface_view_store(&view->sequence, sequence + 1);
  queued(in_surface, BYTES(2, 2), UNAVAILABLE, "the renderer changes a view");
  cpu_surface_view_store(&view->sequence, sequence);
  cpu_surface_view_store(&s_cpu_surface_pending_sequence, cpu_surface_view_load(&s_cpu_surface_pending_sequence) + 1);
  queued(plain, BYTES(2, 2), UNAVAILABLE, "the renderer changes the recorded draws");
  cpu_surface_view_store(&s_cpu_surface_pending_sequence, cpu_surface_view_load(&s_cpu_surface_pending_sequence) - 1);
  served(plain, BYTES(2, 2), "after the renderer finished its change");

  /* The profile record of a served request. */
  s_cpu_surface_profile = 1;
  s_cpu_surface_profile_frequency = 1000;
  served(plain, BYTES(2, 2), "a served request with its profile record");
  served(in_surface, BYTES(2, 2), "a served request in a surface with its profile record");
  served(mixed, BYTES(2, 2), "a served request in a surface and in plain memory with its profile record");
  s_cpu_surface_profile = 0;

  /* B. The renderer changes something between two reads of one request. */
  fixture_hook = hook_new_generation;
  queued(four, BYTES(4, 4), UNAVAILABLE, "the renderer packed a new generation during the read");
  served(four, BYTES(4, 4), "after the pack");
  fixture_hook = hook_targeting_draw;
  queued(four, BYTES(4, 4), UNAVAILABLE, "the renderer recorded a draw that targets the ranges during the read");
  submit_batch();
  fixture_hook = hook_other_draw;
  queued(four, BYTES(4, 4), UNAVAILABLE, "the renderer recorded another draw during the read");
  submit_batch();
  fixture_hook = hook_table_growth;
  queued(four, BYTES(4, 4), UNAVAILABLE, "the view table grew during the read");
  served(four, BYTES(4, 4), "after the table grew");
  /* The boundary path still answers a request that the fast path declines. */
  fixture_boundary = boundary_bytes;
  record_draw(0x1000, 0x40, 1);
  queued(four, BYTES(4, 4), VM_READ_SNAPSHOT_OK, "the boundary holds the bytes of a recorded draw");
  fixture_boundary = boundary_unavailable;
  submit_batch();
  REQUIRE(!fixture_scalar_reads, "the prompt service has nothing to serve in this model");

  /* C. The invariant on two threads. */
  write_surface(surface_a);
  pack_surface(surface_a, 0x90);
  stress();
  puts("PASS: snapshot fast path serves clear ranges on the calling thread, queues every other request, and rechecks after the read");
  return 0;
}
