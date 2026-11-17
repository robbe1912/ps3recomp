/* Authored D3D12 producer exercising the actual backend prefetch/readback path.
 */
#include "backend-static.c"
#include <crtdbg.h>

uint8_t *vm_base;
static ID3D12Fence *active_gate;
static ID3D12DescriptorHeap *target_heap;
static ID3D12CommandAllocator *producer_allocators[16];
static ID3D12GraphicsCommandList *producer_lists[16];
static unsigned producer_count;

u32 rsx_find_vram_upload(u32 bytes) {
  (void)bytes;
  fprintf(stderr, "Unexpected VRAM relocation\n");
  abort();
}

static void require(int condition, const char *expression, int line) {
  if (condition)
    return;
  /* Release only the authored gate, so a failed control leaves no blocked GPU.
   */
  if (active_gate)
    active_gate->lpVtbl->Signal(active_gate, 1);
  fprintf(stderr, "GPU contract failed line %d: %s\n", line, expression);
  exit(7);
}
#define REQUIRE(expression) require(!!(expression), #expression, __LINE__)
#define HR(expression) REQUIRE(SUCCEEDED(expression))

static void initialize_device(void) {
  ID3D12Debug *debug = NULL;
  HR(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&debug));
  debug->lpVtbl->EnableDebugLayer(debug);
  debug->lpVtbl->Release(debug);
  IDXGIFactory4 *factory = NULL;
  IDXGIAdapter *adapter = NULL;
  HR(CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory));
  const char *choice = getenv("RSX_TEST_ADAPTER");
  REQUIRE(choice && (!strcmp(choice, "warp") || !strcmp(choice, "hardware")));
  int warp = !strcmp(choice, "warp");
  if (warp)
    HR(factory->lpVtbl->EnumWarpAdapter(factory, &IID_IDXGIAdapter,
                                        (void **)&adapter));
  HR(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0,
                       &IID_ID3D12Device, (void **)&s_d3d.device));
  if (adapter)
    adapter->lpVtbl->Release(adapter);
  LUID luid;
  s_d3d.device->lpVtbl->GetAdapterLuid(s_d3d.device, &luid);
  IDXGIAdapter1 *actual = NULL;
  DXGI_ADAPTER_DESC1 description;
  HR(factory->lpVtbl->EnumAdapterByLuid(factory, luid, &IID_IDXGIAdapter1,
                                        (void **)&actual));
  HR(actual->lpVtbl->GetDesc1(actual, &description));
  REQUIRE(warp ? (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0
               : (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0);
  printf("Adapter vendor=%04X device=%04X name=%ls\n", description.VendorId,
         description.DeviceId, description.Description);
  actual->lpVtbl->Release(actual);
  factory->lpVtbl->Release(factory);
  D3D12_COMMAND_QUEUE_DESC queue = {0};
  queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  HR(s_d3d.device->lpVtbl->CreateCommandQueue(s_d3d.device, &queue,
                                              &IID_ID3D12CommandQueue,
                                              (void **)&s_d3d.cmd_queue));
  HR(s_d3d.device->lpVtbl->CreateFence(s_d3d.device, 0, D3D12_FENCE_FLAG_NONE,
                                       &IID_ID3D12Fence,
                                       (void **)&s_d3d.fence));
  s_d3d.fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
  REQUIRE(s_d3d.fence_event);
  HR(s_d3d.device->lpVtbl->CreateCommandAllocator(
      s_d3d.device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
      (void **)&s_cpu_surface_alloc));
  HR(s_d3d.device->lpVtbl->CreateCommandList(
      s_d3d.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_cpu_surface_alloc,
      NULL, &IID_ID3D12GraphicsCommandList, (void **)&s_cpu_surface_list));
  HR(s_cpu_surface_list->lpVtbl->Close(s_cpu_surface_list));
  HR(s_d3d.device->lpVtbl->CreateFence(s_d3d.device, 0, D3D12_FENCE_FLAG_NONE,
                                       &IID_ID3D12Fence,
                                       (void **)&s_cpu_surface_fence));
  s_cpu_surface_event = CreateEventA(NULL, FALSE, FALSE, NULL);
  REQUIRE(s_cpu_surface_event);
  D3D12_DESCRIPTOR_HEAP_DESC heap = {0};
  heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  heap.NumDescriptors = 1;
  HR(s_d3d.device->lpVtbl->CreateDescriptorHeap(
      s_d3d.device, &heap, &IID_ID3D12DescriptorHeap, (void **)&target_heap));
  cpu_surface_prefetch_init();
}

static void make_surface(OffRT *surface) {
  memset(surface, 0, sizeof *surface);
  surface->off = 0x1f00;
  surface->w = 4;
  surface->h = 3;
  surface->dxgi = DXGI_FORMAT_R8G8B8A8_UNORM;
  surface->st = D3D12_RESOURCE_STATE_RENDER_TARGET;
  surface->cpu_address = 0x1000;
  surface->cpu_pitch = 8;
  surface->cpu_format = 9; /* Guest B8 uses the host RGBA blue channel. */
  D3D12_HEAP_PROPERTIES heap = {0};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc = {0};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = surface->w;
  desc.Height = surface->h;
  desc.DepthOrArraySize = desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  HR(s_d3d.device->lpVtbl->CreateCommittedResource(
      s_d3d.device, &heap, D3D12_HEAP_FLAG_NONE, &desc, surface->st, NULL,
      &IID_ID3D12Resource, (void **)&surface->res));
  D3D12_CPU_DESCRIPTOR_HANDLE handle;
  target_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(target_heap, &handle);
  s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, surface->res, NULL,
                                               handle);
}

static void write_surface(OffRT *surface, unsigned byte) {
  REQUIRE(producer_count < 16);
  unsigned index = producer_count++;
  HR(s_d3d.device->lpVtbl->CreateCommandAllocator(
      s_d3d.device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
      (void **)&producer_allocators[index]));
  HR(s_d3d.device->lpVtbl->CreateCommandList(
      s_d3d.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
      producer_allocators[index], NULL, &IID_ID3D12GraphicsCommandList,
      (void **)&producer_lists[index]));
  D3D12_CPU_DESCRIPTOR_HANDLE handle;
  target_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(target_heap, &handle);
  const float colour[4] = {0, 0, byte / 255.0f, 1};
  producer_lists[index]->lpVtbl->ClearRenderTargetView(producer_lists[index],
                                                       handle, colour, 0, NULL);
  HR(producer_lists[index]->lpVtbl->Close(producer_lists[index]));
  ID3D12CommandList *list = (ID3D12CommandList *)producer_lists[index];
  s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, &list);
  surface->gpu_generation++;
}

static void read_surface(OffRT *surface, unsigned expected) {
  cpu_surface_read(surface->cpu_address, 1);
  for (unsigned y = 0; y < surface->h; y++) {
    for (unsigned x = 0; x < surface->cpu_pitch; x++)
      REQUIRE(vm_base[surface->cpu_address + y * surface->cpu_pitch + x] ==
              (x < surface->w ? expected : 0xa5));
  }
}

static void hold_queue(void) {
  REQUIRE(!active_gate);
  HR(s_d3d.device->lpVtbl->CreateFence(s_d3d.device, 0, D3D12_FENCE_FLAG_NONE,
                                       &IID_ID3D12Fence,
                                       (void **)&active_gate));
  HR(s_d3d.cmd_queue->lpVtbl->Wait(s_d3d.cmd_queue, active_gate, 1));
}

static DWORD WINAPI release_gate(void *unused) {
  (void)unused;
  Sleep(50);
  return FAILED(active_gate->lpVtbl->Signal(active_gate, 1)) ? 1 : 0;
}

static void finish_gate(HANDLE thread) {
  REQUIRE(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0);
  DWORD code;
  REQUIRE(GetExitCodeThread(thread, &code) && !code);
  CloseHandle(thread);
  active_gate->lpVtbl->Release(active_gate);
  active_gate = NULL;
}

int main(void) {
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  vm_base = malloc(0x10000);
  REQUIRE(vm_base);
  memset(vm_base, 0xa5, 0x10000);
  initialize_device();
  OffRT *surface = &s_d3d.off_rt[0];
  make_surface(surface);
  cpu_surface_prefetch_refresh();
  RsxCpuSurfaceProfile profile = {0};
  s_cpu_surface_profile_active = &profile;
  write_surface(surface, 0x11);
  read_surface(surface, 0x11);
  REQUIRE(profile.copies == 1);
  write_surface(surface, 0x22);
  cpu_surface_prefetch_refresh();
  wait_for_gpu_required();
  read_surface(surface, 0x22);
  int enabled = s_cpu_surface_prefetch_count != 0;
  REQUIRE(profile.copies == (enabled ? 1u : 2u));
  if (enabled)
    REQUIRE(s_cpu_surface_prefetch[0].used == 1);

  /* Completed generation 3 supersedes the prefetched generation 2. */
  write_surface(surface, 0x33);
  wait_for_gpu_required();
  read_surface(surface, 0x33);
  REQUIRE(profile.copies == (enabled ? 2u : 3u));
  if (enabled) {
    CpuSurfacePrefetch *entry = &s_cpu_surface_prefetch[0];
    hold_queue();
    write_surface(surface, 0x44);
    cpu_surface_prefetch_refresh();
    REQUIRE(
        !cpu_surface_fence_reached(s_cpu_surface_prefetch_fence, entry->fence));
    HANDLE worker = CreateThread(NULL, 0, release_gate, NULL, 0, NULL);
    REQUIRE(worker);
    read_surface(surface, 0x44);
    REQUIRE(entry->waited == 1);
    finish_gate(worker);

    /* New guest pitch rejects the old copy and preserves current padding. */
    surface->cpu_pitch = 12;
    surface->cpu_generation = 0;
    memset(vm_base + surface->cpu_address, 0xa5, 36);
    read_surface(surface, 0x44);
    REQUIRE(profile.copies == 3);
    hold_queue();
    write_surface(surface, 0x55);
    cpu_surface_prefetch_refresh();
    u64 retiring_fence = entry->fence;
    u64 retiring_generation = entry->generation;
    worker = CreateThread(NULL, 0, release_gate, NULL, 0, NULL);
    REQUIRE(worker);
    cpu_surface_prefetch_retire(surface);
    REQUIRE(cpu_surface_fence_reached(s_cpu_surface_prefetch_fence,
                                      retiring_fence));
    REQUIRE(entry->slot == -1 && !entry->generation);
    finish_gate(worker);
    if (surface->cpu_readback)
      surface->cpu_readback->lpVtbl->Release(surface->cpu_readback);
    surface->res->lpVtbl->Release(surface->res);
    make_surface(surface);
    surface->gpu_generation = retiring_generation - 1;
    memset(vm_base + surface->cpu_address, 0xa5, 24);
    write_surface(surface, 0x66);
    read_surface(surface, 0x66);
    REQUIRE(profile.copies == 4);
  }
  wait_for_gpu_required();
  cpu_surface_prefetch_shutdown();
  REQUIRE(!s_cpu_surface_prefetch_count && !s_cpu_surface_prefetch_list &&
          !s_cpu_surface_prefetch_alloc && !s_cpu_surface_prefetch_fence &&
          !s_cpu_surface_prefetch_event);
  for (unsigned i = 0; i < CPU_SURFACE_PREFETCH_MAX; i++)
    REQUIRE(!s_cpu_surface_prefetch[i].readback);
  ID3D12InfoQueue *info = NULL;
  HR(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device, &IID_ID3D12InfoQueue,
                                          (void **)&info));
  UINT64 count =
      info->lpVtbl->GetNumStoredMessagesAllowedByRetrievalFilter(info);
  for (UINT64 i = 0; i < count; i++) {
    SIZE_T bytes = 0;
    HR(info->lpVtbl->GetMessage(info, i, NULL, &bytes));
    D3D12_MESSAGE *message = malloc(bytes);
    REQUIRE(message);
    HR(info->lpVtbl->GetMessage(info, i, message, &bytes));
    fprintf(stderr, "D3D12 validation %u id=%u: %s\n", message->Severity,
            message->ID, message->pDescription);
    /* Each generation deliberately clears the same resource to a different
     * value. Its optional optimized-clear hint cannot describe every value. */
    REQUIRE(message->ID ==
                D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE &&
            message->Severity == D3D12_MESSAGE_SEVERITY_WARNING);
    free(message);
  }
  printf("PASS actual GPU coherent prefetch=%d, "
         "generation/layout/fence/retirement/padding; errors=0 "
         "varying-clear-warnings=%llu\n",
         enabled, (unsigned long long)count);
  info->lpVtbl->Release(info);
  for (unsigned i = 0; i < producer_count; i++) {
    producer_lists[i]->lpVtbl->Release(producer_lists[i]);
    producer_allocators[i]->lpVtbl->Release(producer_allocators[i]);
  }
  if (surface->cpu_readback)
    surface->cpu_readback->lpVtbl->Release(surface->cpu_readback);
  surface->res->lpVtbl->Release(surface->res);
  target_heap->lpVtbl->Release(target_heap);
  s_cpu_surface_list->lpVtbl->Release(s_cpu_surface_list);
  s_cpu_surface_alloc->lpVtbl->Release(s_cpu_surface_alloc);
  s_cpu_surface_fence->lpVtbl->Release(s_cpu_surface_fence);
  CloseHandle(s_cpu_surface_event);
  s_d3d.fence->lpVtbl->Release(s_d3d.fence);
  CloseHandle(s_d3d.fence_event);
  s_d3d.cmd_queue->lpVtbl->Release(s_d3d.cmd_queue);
  s_d3d.device->lpVtbl->Release(s_d3d.device);
  free(vm_base);
  return 0;
}
