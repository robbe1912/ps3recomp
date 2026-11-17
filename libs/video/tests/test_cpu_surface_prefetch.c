/* Fake D3D12 GPU timeline for the CPU surface read path. The production
 * coherent path, fence helper and surface prefetch are injected unchanged;
 * only the D3D12/Win32 calls are modelled. */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <crtdbg.h>
#endif
typedef struct VmReadSnapshotRange VmReadSnapshotRange;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t HRESULT;
typedef int32_t LONG;
typedef uint32_t DWORD;
typedef size_t SIZE_T;
typedef int DXGI_FORMAT;
typedef void *HANDLE;
#define FAILED(h) ((HRESULT)(h) < 0)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define DEVICE_REMOVED ((HRESULT)0x887A0005L)
#define FALSE 0
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 0x102u
typedef struct {
  void *p;
} SRWLOCK;
#define SRWLOCK_INIT {0}
typedef union {
  long long QuadPart;
} LARGE_INTEGER;
static int QueryPerformanceCounter(LARGE_INTEGER *t) {
  static long long c;
  t->QuadPart = ++c;
  return 1;
}
#define REQUIRE(x, message)                                                    \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__);              \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

/* ---- D3D12 subset ---- */
typedef struct {
  SIZE_T Begin, End;
} D3D12_RANGE;
enum {
  D3D12_HEAP_TYPE_READBACK = 3,
  D3D12_RESOURCE_DIMENSION_BUFFER = 1,
  D3D12_TEXTURE_LAYOUT_ROW_MAJOR = 1,
  D3D12_HEAP_FLAG_NONE = 0,
  D3D12_RESOURCE_STATE_COPY_DEST = 0x400,
  D3D12_RESOURCE_STATE_COPY_SOURCE = 0x800,
  D3D12_RESOURCE_STATE_RENDER_TARGET = 4,
  D3D12_COMMAND_LIST_TYPE_DIRECT = 0,
  D3D12_FENCE_FLAG_NONE = 0,
  D3D12_RESOURCE_BARRIER_TYPE_TRANSITION = 0,
  D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES = 0xffffffff,
  D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX = 0,
  D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT = 1,
  D3D12_TEXTURE_DATA_PITCH_ALIGNMENT = 256
};
typedef struct {
  int Type;
} D3D12_HEAP_PROPERTIES;
typedef struct {
  int Dimension;
  u64 Width;
  u32 Height;
  unsigned short DepthOrArraySize, MipLevels;
  struct {
    u32 Count;
  } SampleDesc;
  int Layout;
} D3D12_RESOURCE_DESC;
typedef struct ID3D12Resource ID3D12Resource;
typedef struct {
  int Type;
  struct {
    ID3D12Resource *pResource;
    u32 Subresource;
    int StateBefore, StateAfter;
  } Transition;
} D3D12_RESOURCE_BARRIER;
typedef struct {
  ID3D12Resource *pResource;
  int Type;
  struct {
    u64 Offset;
    struct {
      DXGI_FORMAT Format;
      u32 Width, Height, Depth, RowPitch;
    } Footprint;
  } PlacedFootprint;
} D3D12_TEXTURE_COPY_LOCATION;
typedef struct {
  int unused;
} GUID;
static const GUID IID_ID3D12Resource, IID_ID3D12CommandAllocator,
    IID_ID3D12GraphicsCommandList, IID_ID3D12Fence;

/* Resources: a surface holds one byte value everywhere; a readback buffer holds
 * bytes. */
typedef struct ResourceVtbl {
  HRESULT (*Map)(ID3D12Resource *, u32, const D3D12_RANGE *, void **);
  void (*Unmap)(ID3D12Resource *, u32, const D3D12_RANGE *);
  unsigned (*Release)(ID3D12Resource *);
} ResourceVtbl;
struct ID3D12Resource {
  const ResourceVtbl *lpVtbl;
  int surface;
  unsigned char value;
  u32 w, h, host_bpp;
  unsigned char *data;
  u64 bytes;
  int released;
};
static HRESULT res_map(ID3D12Resource *r, u32 s, const D3D12_RANGE *range,
                       void **out);
static void res_unmap(ID3D12Resource *r, u32 s, const D3D12_RANGE *range) {
  (void)r;
  (void)s;
  (void)range;
}
static unsigned res_release(ID3D12Resource *r) {
  REQUIRE(!r->released, "resource released twice");
  r->released = 1;
  return 0;
}
static const ResourceVtbl s_res_vtbl = {res_map, res_unmap, res_release};

/* GPU timeline: executed in submission order, only when the test lets it run.
 */
typedef struct ID3D12Fence ID3D12Fence;
enum { OP_WRITE, OP_COPY, OP_SIGNAL };
typedef struct {
  int kind;
  ID3D12Resource *src, *dst;
  u32 pitch;
  unsigned char value;
  ID3D12Fence *fence;
  u64 fence_value;
} Op;
static Op gpu_ops[4096];
static unsigned gpu_submitted, gpu_executed;
static int gpu_removed, remove_on_wait; /* remove_on_wait: the device is lost
                                           while the host waits */
static unsigned host_waits;

typedef struct FenceVtbl {
  u64 (*GetCompletedValue)(ID3D12Fence *);
  HRESULT (*SetEventOnCompletion)(ID3D12Fence *, u64, HANDLE);
  unsigned (*Release)(ID3D12Fence *);
} FenceVtbl;
struct ID3D12Fence {
  const FenceVtbl *lpVtbl;
  u64 completed;
};
typedef struct {
  ID3D12Fence *fence;
  u64 value;
  int armed;
} FakeEvent;
static u64 fence_completed(ID3D12Fence *f) {
  return gpu_removed ? UINT64_MAX : f->completed;
}
static HRESULT fence_set_event(ID3D12Fence *f, u64 v, HANDLE e) {
  FakeEvent *ev = (FakeEvent *)e;
  ev->fence = f;
  ev->value = v;
  ev->armed = 1;
  return 0;
}
static unsigned fence_release(ID3D12Fence *f) {
  (void)f;
  return 0;
}
static const FenceVtbl s_fence_vtbl = {fence_completed, fence_set_event,
                                       fence_release};

static void gpu_step(void) {
  Op *op = &gpu_ops[gpu_executed++];
  if (op->kind == OP_WRITE)
    op->dst->value = op->value;
  else if (op->kind == OP_SIGNAL)
    op->fence->completed = op->fence_value;
  else {
    ID3D12Resource *s = op->src, *d = op->dst;
    REQUIRE((u64)op->pitch * s->h <= d->bytes, "copy fits its readback");
    for (u32 y = 0; y < s->h; y++) {
      memset(d->data + (size_t)y * op->pitch, s->value,
             (size_t)s->w * s->host_bpp);
      memset(d->data + (size_t)y * op->pitch + (size_t)s->w * s->host_bpp, 0xCD,
             op->pitch - (size_t)s->w * s->host_bpp);
    }
  }
}
static void gpu_run(void) {
  while (gpu_executed < gpu_submitted)
    gpu_step();
}
static void gpu_push(Op op) {
  REQUIRE(gpu_submitted < 4096, "op space");
  gpu_ops[gpu_submitted++] = op;
}

static HANDLE CreateEventA(void *a, int b, int c, void *d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  return calloc(1, sizeof(FakeEvent));
}
static int CloseHandle(HANDLE h) {
  free(h);
  return 1;
}
/* Blocking: the host thread sleeps while the GPU runs forward. */
static DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
  (void)ms;
  FakeEvent *ev = (FakeEvent *)h;
  host_waits++;
  REQUIRE(ev->armed, "wait on an unarmed event");
  ev->armed = 0;
  if (remove_on_wait)
    gpu_removed = 1;
  if (gpu_removed)
    return WAIT_OBJECT_0; /* removal signals every fence event */
  while (ev->fence->completed < ev->value && gpu_executed < gpu_submitted)
    gpu_step();
  return ev->fence->completed >= ev->value ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
}

/* Command lists record; the queue appends their commands to the timeline. */
typedef struct ID3D12CommandAllocator ID3D12CommandAllocator;
typedef struct AllocVtbl {
  HRESULT (*Reset)(ID3D12CommandAllocator *);
  unsigned (*Release)(ID3D12CommandAllocator *);
} AllocVtbl;
struct ID3D12CommandAllocator {
  const AllocVtbl *lpVtbl;
  unsigned busy_until;
};
static HRESULT alloc_reset(ID3D12CommandAllocator *a) {
  REQUIRE(gpu_executed >= a->busy_until,
          "allocator reset while the GPU still executes its commands");
  return 0;
}
static unsigned alloc_release(ID3D12CommandAllocator *a) {
  (void)a;
  return 0;
}
static const AllocVtbl s_alloc_vtbl = {alloc_reset, alloc_release};
typedef struct ID3D12CommandList ID3D12CommandList;
typedef struct ID3D12GraphicsCommandList ID3D12GraphicsCommandList;
typedef struct ListVtbl {
  HRESULT (*Reset)(ID3D12GraphicsCommandList *, ID3D12CommandAllocator *,
                   void *);
  HRESULT (*Close)(ID3D12GraphicsCommandList *);
  void (*ResourceBarrier)(ID3D12GraphicsCommandList *, u32,
                          const D3D12_RESOURCE_BARRIER *);
  void (*CopyTextureRegion)(ID3D12GraphicsCommandList *,
                            const D3D12_TEXTURE_COPY_LOCATION *, u32, u32, u32,
                            const D3D12_TEXTURE_COPY_LOCATION *, const void *);
  unsigned (*Release)(ID3D12GraphicsCommandList *);
} ListVtbl;
struct ID3D12GraphicsCommandList {
  const ListVtbl *lpVtbl;
  ID3D12CommandAllocator *alloc;
  Op ops[8];
  unsigned count;
  int open;
  unsigned copies;
  int barrier_state;
};
static HRESULT list_reset(ID3D12GraphicsCommandList *l,
                          ID3D12CommandAllocator *a, void *p) {
  (void)p;
  REQUIRE(!l->open, "list reset while open");
  l->alloc = a;
  l->count = 0;
  l->open = 1;
  return 0;
}
static HRESULT list_close(ID3D12GraphicsCommandList *l) {
  l->open = 0;
  return 0;
}
static void list_barrier(ID3D12GraphicsCommandList *l, u32 n,
                         const D3D12_RESOURCE_BARRIER *b) {
  REQUIRE(n == 1 && l->open, "one barrier in an open list");
  if (b->Transition.StateAfter == D3D12_RESOURCE_STATE_COPY_SOURCE)
    l->barrier_state = 1;
  else {
    REQUIRE(l->barrier_state, "barrier restored");
    l->barrier_state = 0;
  }
}
static void list_copy(ID3D12GraphicsCommandList *l,
                      const D3D12_TEXTURE_COPY_LOCATION *dst, u32 x, u32 y,
                      u32 z, const D3D12_TEXTURE_COPY_LOCATION *src,
                      const void *box) {
  (void)x;
  (void)y;
  (void)z;
  (void)box;
  REQUIRE(l->open && l->count < 8, "copy in an open list");
  REQUIRE(src->pResource->surface && !dst->pResource->surface,
          "surface to readback copy");
  REQUIRE(dst->PlacedFootprint.Footprint.Width == src->pResource->w &&
              dst->PlacedFootprint.Footprint.Height == src->pResource->h,
          "copy footprint");
  Op op = {OP_COPY,
           src->pResource,
           dst->pResource,
           dst->PlacedFootprint.Footprint.RowPitch,
           0,
           NULL,
           0};
  l->ops[l->count++] = op;
  l->copies++;
}
static unsigned list_release(ID3D12GraphicsCommandList *l) {
  (void)l;
  return 0;
}
static const ListVtbl s_list_vtbl = {list_reset, list_close, list_barrier,
                                     list_copy, list_release};

typedef struct ID3D12CommandQueue ID3D12CommandQueue;
typedef struct QueueVtbl {
  void (*ExecuteCommandLists)(ID3D12CommandQueue *, u32,
                              ID3D12CommandList *const *);
  HRESULT (*Signal)(ID3D12CommandQueue *, ID3D12Fence *, u64);
} QueueVtbl;
struct ID3D12CommandQueue {
  const QueueVtbl *lpVtbl;
};
static void queue_execute(ID3D12CommandQueue *q, u32 n,
                          ID3D12CommandList *const *lists) {
  (void)q;
  for (u32 i = 0; i < n; i++) {
    ID3D12GraphicsCommandList *l = (ID3D12GraphicsCommandList *)lists[i];
    REQUIRE(!l->open, "executed list is closed");
    for (unsigned j = 0; j < l->count; j++)
      gpu_push(l->ops[j]);
    l->alloc->busy_until = gpu_submitted;
  }
}
static HRESULT queue_signal(ID3D12CommandQueue *q, ID3D12Fence *f, u64 v) {
  (void)q;
  Op op = {OP_SIGNAL, NULL, NULL, 0, 0, f, v};
  gpu_push(op);
  return 0;
}
static const QueueVtbl s_queue_vtbl = {queue_execute, queue_signal};

typedef struct ID3D12Device ID3D12Device;
typedef struct DeviceVtbl {
  HRESULT (*CreateCommittedResource)(ID3D12Device *,
                                     const D3D12_HEAP_PROPERTIES *, int,
                                     const D3D12_RESOURCE_DESC *, int,
                                     const void *, const GUID *, void **);
  HRESULT (*CreateCommandAllocator)(ID3D12Device *, int, const GUID *, void **);
  HRESULT (*CreateCommandList)(ID3D12Device *, u32, int,
                               ID3D12CommandAllocator *, void *, const GUID *,
                               void **);
  HRESULT (*CreateFence)(ID3D12Device *, u64, int, const GUID *, void **);
  HRESULT (*GetDeviceRemovedReason)(ID3D12Device *);
} DeviceVtbl;
struct ID3D12Device {
  const DeviceVtbl *lpVtbl;
};
static unsigned readbacks_created;
static HRESULT dev_resource(ID3D12Device *d, const D3D12_HEAP_PROPERTIES *hp,
                            int flags, const D3D12_RESOURCE_DESC *desc,
                            int state, const void *clear, const GUID *iid,
                            void **out) {
  (void)d;
  (void)flags;
  (void)state;
  (void)clear;
  (void)iid;
  REQUIRE(hp->Type == D3D12_HEAP_TYPE_READBACK &&
              desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER,
          "readback buffer");
  ID3D12Resource *r = calloc(1, sizeof *r);
  r->lpVtbl = &s_res_vtbl;
  r->bytes = desc->Width;
  r->data = malloc((size_t)r->bytes);
  memset(r->data, 0xAB, (size_t)r->bytes);
  readbacks_created++;
  *out = r;
  return 0;
}
static HRESULT dev_alloc(ID3D12Device *d, int t, const GUID *iid, void **out) {
  (void)d;
  (void)t;
  (void)iid;
  ID3D12CommandAllocator *a = calloc(1, sizeof *a);
  a->lpVtbl = &s_alloc_vtbl;
  *out = a;
  return 0;
}
static HRESULT dev_list(ID3D12Device *d, u32 m, int t,
                        ID3D12CommandAllocator *a, void *p, const GUID *iid,
                        void **out) {
  (void)d;
  (void)m;
  (void)t;
  (void)p;
  (void)iid;
  ID3D12GraphicsCommandList *l = calloc(1, sizeof *l);
  l->lpVtbl = &s_list_vtbl;
  l->alloc = a;
  l->open = 1;
  *out = l;
  return 0;
}
static HRESULT dev_fence(ID3D12Device *d, u64 v, int f, const GUID *iid,
                         void **out) {
  (void)d;
  (void)f;
  (void)iid;
  ID3D12Fence *x = calloc(1, sizeof *x);
  x->lpVtbl = &s_fence_vtbl;
  x->completed = v;
  *out = x;
  return 0;
}
static HRESULT dev_removed(ID3D12Device *d) {
  (void)d;
  return gpu_removed ? DEVICE_REMOVED : 0;
}
static const DeviceVtbl s_dev_vtbl = {dev_resource, dev_alloc, dev_list,
                                      dev_fence, dev_removed};

static HRESULT res_map(ID3D12Resource *r, u32 s, const D3D12_RANGE *range,
                       void **out) {
  (void)s;
  (void)range;
  REQUIRE(!r->surface, "only readbacks are mapped");
  for (unsigned i = gpu_executed; i < gpu_submitted; i++)
    REQUIRE(!(gpu_ops[i].kind == OP_COPY && gpu_ops[i].dst == r),
            "mapped a readback the GPU is still copying into");
  *out = r->data;
  return 0;
}

typedef struct {
  ID3D12Resource *res;
  u32 off, w, h, dxgi;
  int st;
  u32 cpu_address, cpu_pitch, cpu_format;
  u64 gpu_generation, cpu_generation;
  ID3D12Resource *cpu_readback;
} OffRT;
#define CPU_SURFACE_CAPACITY 4
static struct {
  ID3D12Device *device;
  ID3D12CommandQueue *cmd_queue;
  OffRT off_rt[CPU_SURFACE_CAPACITY];
} s_d3d;
uint8_t *vm_base;
static const char *fixture_prefetch_env;
static const char *fixture_getenv(const char *name) {
  return !strcmp(name, "RSX_CPU_SURFACE_PREFETCH") ? fixture_prefetch_env
                                                   : NULL;
}
#define getenv fixture_getenv
#include "rsx_cpu_surface_profile.h"
#include "rsx_surface_readback.h"
static u64 GetTickCount64(void) {
  static u64 ticks;
  return ++ticks;
}
static void rsx_gpu_fault_report(ID3D12Device *device) { (void)device; }

/* PRODUCTION */
#undef getenv

/* ---- Scenario helpers ---- */
static ID3D12Device s_device = {&s_dev_vtbl};
static ID3D12CommandQueue s_queue = {&s_queue_vtbl};
static void boot(const char *list) {
  s_d3d.device = &s_device;
  s_d3d.cmd_queue = &s_queue;
  s_d3d.device->lpVtbl->CreateCommandAllocator(s_d3d.device, 0,
                                               &IID_ID3D12CommandAllocator,
                                               (void **)&s_cpu_surface_alloc);
  s_d3d.device->lpVtbl->CreateCommandList(
      s_d3d.device, 0, 0, s_cpu_surface_alloc, NULL,
      &IID_ID3D12GraphicsCommandList, (void **)&s_cpu_surface_list);
  s_cpu_surface_list->lpVtbl->Close(s_cpu_surface_list);
  s_d3d.device->lpVtbl->CreateFence(s_d3d.device, 0, 0, &IID_ID3D12Fence,
                                    (void **)&s_cpu_surface_fence);
  s_cpu_surface_event = CreateEventA(NULL, FALSE, FALSE, NULL);
  fixture_prefetch_env = list;
  cpu_surface_prefetch_init();
}
static OffRT *surface(int slot, u32 off, u32 address) {
  OffRT *r = &s_d3d.off_rt[slot];
  memset(r, 0, sizeof *r);
  r->res = calloc(1, sizeof *r->res);
  r->res->lpVtbl = &s_res_vtbl;
  r->res->surface = 1;
  r->res->w = 4;
  r->res->h = 2;
  r->res->host_bpp = 8;
  r->off = off;
  r->w = 4;
  r->h = 2;
  r->st = D3D12_RESOURCE_STATE_RENDER_TARGET;
  r->cpu_address = address;
  r->cpu_pitch = 32;
  r->cpu_format = 11;
  return r;
}
/* One submitted batch that wrote r with value v, then the per-batch refresh. */
static void batch(OffRT *r, unsigned char v) {
  Op op = {OP_WRITE, NULL, r->res, 0, v, NULL, 0};
  gpu_push(op);
  r->gpu_generation++;
  cpu_surface_prefetch_refresh();
}
static void idle_batch(void) { cpu_surface_prefetch_refresh(); }
static unsigned coherent_copies(void) {
  return ((ID3D12GraphicsCommandList *)s_cpu_surface_list)->copies;
}
static unsigned prefetch_copies(void) {
  return ((ID3D12GraphicsCommandList *)s_cpu_surface_prefetch_list)->copies;
}
/* Guest memory keeps whatever the last copy packed; every generation has its
 * own value. */
static unsigned char read_byte(OffRT *r, u32 at) {
  cpu_surface_read(r->cpu_address + at, 1);
  return vm_base[r->cpu_address + at];
}
static CpuSurfacePrefetch *entry0(void) { return &s_cpu_surface_prefetch[0]; }

static int run_config(const char *list) {
  fixture_prefetch_env = list;
  cpu_surface_prefetch_configure(list);
  return 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  static uint8_t memory[0x10000];
  vm_base = memory;
  if (argc == 3 && !strcmp(argv[1], "config"))
    return run_config(argv[2]);
  if (argc == 2 && !strcmp(argv[1], "removed-prefetch")) {
    boot("1F00");
    OffRT *r = surface(0, 0x1F00, 0x1000);
    idle_batch();
    read_byte(r, 0);
    batch(r, 0x11);
    REQUIRE(prefetch_copies() == 1, "copy queued");
    gpu_removed = 1;
    read_byte(r, 0);
    fprintf(stderr, "FAIL: device removal treated as a completed prefetch\n");
    return 1;
  }
  if (argc == 2 && !strcmp(argv[1], "removed-refresh")) {
    boot("1F00");
    OffRT *r = surface(0, 0x1F00, 0x1000);
    idle_batch();
    read_byte(r, 0);
    batch(r, 0x11);
    cpu_surface_prefetch_note(r);
    gpu_removed = 1;
    batch(r, 0x22);
    fprintf(stderr, "FAIL: device removal treated as a completed prefetch\n");
    return 1;
  }
  if (argc == 2 && !strcmp(argv[1], "removed-coherent")) {
    boot("");
    OffRT *r = surface(0, 0x1F00, 0x1000);
    batch(r, 0x11);
    gpu_removed = 1;
    read_byte(r, 0);
    fprintf(stderr,
            "FAIL: device removal treated as a completed coherent copy\n");
    return 1;
  }
  if (argc == 2 && !strcmp(argv[1], "removed-while-waiting")) {
    boot("1F00");
    OffRT *r = surface(0, 0x1F00, 0x1000);
    idle_batch();
    read_byte(r, 0);
    batch(r, 0x11);
    remove_on_wait = 1;
    read_byte(r, 0);
    fprintf(
        stderr,
        "FAIL: device removed during the prefetch wait treated as completed\n");
    return 1;
  }
  if (argc == 2 && !strcmp(argv[1], "recording")) {
    boot("1F00");
    s_cpu_surface_recording = 1;
    cpu_surface_prefetch_refresh();
    return 0;
  }
  /* Parsing (the stopping cases run in their own processes). */
  run_config("");
  REQUIRE(s_cpu_surface_prefetch_count == 0, "empty list enables nothing");
  run_config(NULL);
  REQUIRE(s_cpu_surface_prefetch_count == 0, "unset enables nothing");
  run_config("02A30480,1f00,FFFFFFFF");
  REQUIRE(s_cpu_surface_prefetch_count == 3 &&
              s_cpu_surface_prefetch[0].off == 0x02A30480 &&
              s_cpu_surface_prefetch[1].off == 0x1F00 &&
              s_cpu_surface_prefetch[2].off == 0xFFFFFFFFu &&
              s_cpu_surface_prefetch[0].slot == -1,
          "three hex offsets parsed");

  boot("1F00");
  OffRT *r = surface(0, 0x1F00, 0x1000);
  /* Generation 1 written; nothing read yet, so nothing prefetched. */
  batch(r, 0x11);
  REQUIRE(prefetch_copies() == 0, "no prefetch before the surface is read");
  REQUIRE(read_byte(r, 8) == 0x11 && coherent_copies() == 1,
          "first read is a coherent copy of generation 1");
  REQUIRE(read_byte(r, 8) == 0x11 && coherent_copies() == 1,
          "same generation is a hit");
  /* The read asked for the next generation to be copied ahead. */
  batch(r, 0x22);
  REQUIRE(prefetch_copies() == 1 && entry0()->generation == 2,
          "generation 2 prefetched behind its batch");
  gpu_run();
  unsigned waits = host_waits;
  REQUIRE(read_byte(r, 8) == 0x22 && coherent_copies() == 1 &&
              host_waits == waits,
          "completed prefetch serves generation 2 without a copy or a wait");
  REQUIRE(entry0()->used == 1 && entry0()->waited == 0,
          "prefetch counted as used");
  REQUIRE(read_byte(r, 0) == 0x22 && read_byte(r, 63) == 0x22,
          "every byte of the surface packed");
  /* Review reproduction: a newer generation completes after the prefetched
   * one. The read must observe it, exactly like the coherent control. */
  batch(r, 0x33);
  REQUIRE(prefetch_copies() == 2,
          "generation 3 prefetched (read since the last copy)");
  gpu_run();
  batch(r, 0x44);
  REQUIRE(prefetch_copies() == 2,
          "generation 4 not prefetched (no read since)");
  gpu_run();
  unsigned long long missed = entry0()->missed;
  REQUIRE(read_byte(r, 8) == 0x44, "read after generation 4 completed observes "
                                   "generation 4, never the prefetched 3");
  REQUIRE(coherent_copies() == 2 && entry0()->missed == missed + 1,
          "older prefetch falls back to a coherent copy");
  /* Prefetch still on the GPU: the read waits for that copy only. */
  batch(r, 0x55);
  REQUIRE(prefetch_copies() == 3, "generation 5 prefetched");
  waits = host_waits;
  REQUIRE(read_byte(r, 8) == 0x55 && coherent_copies() == 2 &&
              host_waits == waits + 1 && entry0()->waited == 1,
          "in-flight prefetch waited for, then served");
  /* Writes submitted after the prefetch was queued make it stale. */
  batch(r, 0x66);
  gpu_run();
  REQUIRE(prefetch_copies() == 4, "generation 6 prefetched");
  Op op = {OP_WRITE, NULL, r->res, 0, 0x77, NULL, 0};
  gpu_push(op);
  r->gpu_generation++;
  REQUIRE(read_byte(r, 8) == 0x77 && coherent_copies() == 3,
          "write after the prefetch: coherent copy of generation 7");
  /* The allocator is reset only once its previous copies finished. */
  batch(r, 0x88);
  REQUIRE(prefetch_copies() == 5, "generation 8 prefetched");
  REQUIRE(read_byte(r, 8) == 0x88, "generation 8 served (after waiting)");
  gpu_run();
  batch(r, 0x99);
  REQUIRE(prefetch_copies() == 6, "generation 9 prefetched");
  /* The GPU has not reached that copy; a read since wants the next one. */
  cpu_surface_prefetch_note(r);
  batch(r, 0xA1);
  REQUIRE(prefetch_copies() == 6,
          "no new copy while the allocator still backs the last one");
  REQUIRE(read_byte(r, 8) == 0xA1 && coherent_copies() == 4,
          "read still sees the newest generation, coherently");
  /* A changed layout never uses the old copy. */
  gpu_run();
  batch(r, 0xB1);
  gpu_run();
  REQUIRE(prefetch_copies() == 7, "generation B1 prefetched");
  r->cpu_pitch = 64;
  unsigned coherent = coherent_copies();
  REQUIRE(read_byte(r, 8) == 0xB1 && coherent_copies() == coherent + 1,
          "changed pitch takes the coherent copy");
  r->cpu_pitch = 32;
  r->cpu_generation = 0;
  REQUIRE(read_byte(r, 8) == 0xB1 && coherent_copies() == coherent + 1,
          "original layout served from the prefetch again");
  /* Retirement waits for a copy in flight and forgets it. */
  batch(r, 0xC1);
  REQUIRE(entry0()->generation == r->gpu_generation,
          "copy in flight before retirement");
  u64 old_generation = entry0()->generation;
  unsigned retire_waits = host_waits;
  cpu_surface_prefetch_retire(r);
  REQUIRE(host_waits == retire_waits + 1 && entry0()->slot == -1 &&
              entry0()->generation == 0,
          "retire waits for the copy and untracks");
  /* A new surface in the same slot restarts its generations: never serve the
   * old copy. */
  OffRT *n = surface(0, 0x1F00, 0x1000);
  {
    Op w = {OP_WRITE, NULL, n->res, 0, 0xD1, NULL, 0};
    gpu_push(w);
    n->gpu_generation = old_generation;
  }
  coherent = coherent_copies();
  REQUIRE(read_byte(n, 8) == 0xD1 && coherent_copies() == coherent + 1,
          "new surface with a reused generation number read coherently");
  /* Unlisted surfaces are never prefetched. */
  OffRT *u = surface(2, 0x5000, 0x3000);
  unsigned listed = prefetch_copies();
  batch(u, 0xE1);
  read_byte(u, 0);
  batch(u, 0xE2);
  REQUIRE(prefetch_copies() == listed && read_byte(u, 0) == 0xE2,
          "unlisted surface stays coherent and unprefetched");
  cpu_surface_prefetch_shutdown();
  REQUIRE(s_cpu_surface_prefetch_count == 0, "shutdown clears the list");
  puts("PASS: coherent surface prefetch, fence failure and retirement");
  return 0;
}
