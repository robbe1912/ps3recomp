/* Authored headless D3D12 producer exercising the actual CPU surface service.
 * The fixture supplies closed-boundary scheduling; FIFO parsing is tested by
 * test_fifo_snapshot_boundary.py. No game data or window is required. */
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <crtdbg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
typedef uint64_t u64;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "CHECK %s at line %d\n", #condition, __LINE__); exit(7); \
} } while (0)
#define HR(call) do { HRESULT hr = (call); if (FAILED(hr)) { \
    fprintf(stderr, "HRESULT %s: %08lX\n", #call, (unsigned long)hr); exit(8); \
} } while (0)
#define CPU_SURFACE_CAPACITY 3
#define CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER 1
#define CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER 2
typedef struct {
    ID3D12Resource *res, *cpu_readback;
    u32 cpu_address, cpu_pitch, cpu_format, w, h, dxgi, off;
    u64 gpu_generation, cpu_generation;
    D3D12_RESOURCE_STATES st;
} OffRT;
typedef struct {
    int in_begin_end;
    u32 color_target, surface_color_dma[4], surface_color_offset[4], surface_color_pitch[4];
} rsx_state;
typedef struct { u32 cpu_address[4], cpu_pitch[4], rt_fmt; } D3D12DrawRecord;
static rsx_state state;
static struct {
    ID3D12Device *device;
    ID3D12CommandQueue *cmd_queue;
    OffRT off_rt[CPU_SURFACE_CAPACITY];
    const rsx_state *current_rsx_state;
    u32 draw_count;
} s_d3d;
static uint8_t memory[0x10000];
uint8_t *vm_base = memory;
static HANDLE kick, control, controlled, ready, stopping, paused, resume;
static void (*operation)(void);
void *cellGcm_fifo_kick_event(void) { return kick; }
u32 cellGcmResolveLocated(int local, u32 offset) { (void)local; return offset; }
#include "rsx_d3d12_fault.h"
// PRODUCTION_SURFACE

static ID3D12CommandAllocator *producer_alloc;
static ID3D12GraphicsCommandList *producer_list;
static ID3D12Resource *upload;
static ID3D12Fence *gate;
static u32 width = 4, height = 3, pitch = 8, pattern = 1;
static int change_kind, failure_kind;
static struct { u64 gpu, cpu, fence; } observed;
static uint8_t blue(u32 x, u32 y) { return (uint8_t)(pattern * 17 + x * 3 + y * 29); }

static void release_upload(void) {
    if (upload) { upload->lpVtbl->Release(upload); upload = NULL; }
    if (producer_list) { producer_list->lpVtbl->Release(producer_list); producer_list = NULL; }
    if (producer_alloc) { producer_alloc->lpVtbl->Release(producer_alloc); producer_alloc = NULL; }
}
static void make_target(void) {
    OffRT *r = &s_d3d.off_rt[0];
    CHECK(!r->res && !r->cpu_readback);
    D3D12_HEAP_PROPERTIES heap = {0}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {0};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    HR(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &heap, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&r->res));
    r->w = width; r->h = height; r->dxgi = desc.Format; r->st = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12DrawRecord draw = {0}; draw.cpu_address[0] = 0x1000; draw.cpu_pitch[0] = pitch; draw.rt_fmt = 9;
    cpu_surface_bind(0, &draw, 0);
}
static void produce(void) {
    OffRT *r = &s_d3d.off_rt[0];
    CHECK(r->res && !upload);
    D3D12_HEAP_PROPERTIES heap = {0}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc = {0}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256 * height; desc.Height = 1; desc.DepthOrArraySize = 1;
    desc.MipLevels = 1; desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HR(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &heap, D3D12_HEAP_FLAG_NONE,
        &desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&upload));
    uint8_t *mapped = NULL; D3D12_RANGE empty = {0, 0};
    HR(upload->lpVtbl->Map(upload, 0, &empty, (void **)&mapped));
    memset(mapped, 0, (size_t)desc.Width);
    for (u32 y = 0; y < height; ++y) for (u32 x = 0; x < width; ++x) {
        uint8_t *pixel = mapped + y * 256 + x * 4;
        pixel[0] = (uint8_t)(3 + x); pixel[1] = (uint8_t)(7 + y);
        pixel[2] = blue(x, y); pixel[3] = 255;
    }
    upload->lpVtbl->Unmap(upload, 0, NULL);
    HR(s_d3d.device->lpVtbl->CreateCommandAllocator(s_d3d.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void **)&producer_alloc));
    HR(s_d3d.device->lpVtbl->CreateCommandList(s_d3d.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        producer_alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&producer_list));
    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = (D3D12_SUBRESOURCE_FOOTPRINT){DXGI_FORMAT_R8G8B8A8_UNORM, width, height, 1, 256};
    dst.pResource = r->res; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    producer_list->lpVtbl->CopyTextureRegion(producer_list, &dst, 0, 0, 0, &src, NULL);
    HR(producer_list->lpVtbl->Close(producer_list));
    ID3D12CommandList *lists[] = {(ID3D12CommandList *)producer_list};
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, lists);
    const int secondary[3] = {-1, -1, -1}; cpu_surface_written(0, secondary);
}
static void probe(void) {
    observed.gpu = s_d3d.off_rt[0].gpu_generation;
    observed.cpu = s_d3d.off_rt[0].cpu_generation;
    observed.fence = s_cpu_surface_fence_value;
}
static void change(void) {
    OffRT *r = &s_d3d.off_rt[0];
    switch (change_kind) {
    case 0: state.in_begin_end = 0; s_d3d.draw_count = 0; s_d3d.current_rsx_state = &state; break;
    case 1: state.in_begin_end = 1; break;
    case 2: s_d3d.draw_count = 1; break;
    case 3:
        s_d3d.off_rt[1] = *r;
        s_d3d.off_rt[1].cpu_readback = NULL;
        s_d3d.off_rt[1].cpu_address = 0x1001;
        s_d3d.off_rt[1].cpu_pitch = s_d3d.off_rt[1].h = 1;
        r->res->lpVtbl->AddRef(r->res); break;
    case 4:
        s_d3d.off_rt[1].res->lpVtbl->Release(s_d3d.off_rt[1].res);
        memset(&s_d3d.off_rt[1], 0, sizeof(OffRT)); break;
    case 5:
        cpu_surface_retire(r); CHECK(!r->cpu_readback && !r->cpu_address);
        r->res->lpVtbl->Release(r->res); r->res = NULL; release_upload(); break;
    case 6:
        HR(s_d3d.cmd_queue->lpVtbl->Wait(s_d3d.cmd_queue, gate, 1));
        CHECK(SetEvent(s_cpu_surface_event)); break;
    case 7:
        if (failure_kind == 1) s_cpu_surface_recording = 1;
        else if (failure_kind == 2) {
            ID3D12Device5 *device5 = NULL;
            HR(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device, &IID_ID3D12Device5, (void **)&device5));
            device5->lpVtbl->RemoveDevice(device5); device5->lpVtbl->Release(device5);
        } else r->cpu_format = 31;
        break;
    case 8: s_d3d.current_rsx_state = NULL; break;
    }
}
static void retire_pending(void) {
    CHECK(SetEvent(paused)); CHECK(WaitForSingleObject(resume, 10000) == WAIT_OBJECT_0);
    change_kind = 5; change();
}
static void direct_rejection(void) {
    const VmReadSnapshotRange range = {0x1000, 1, 0}; uint8_t output = 0xa5;
    CHECK(vm_read_snapshot(&range, 1, &output, 1) == VM_READ_SNAPSHOT_UNAVAILABLE && output == 0xa5);
}
static DWORD WINAPI owner(void *unused) {
    (void)unused; s_d3d.current_rsx_state = &state; cpu_surface_init();
    cpu_surface_service(0); CHECK(SetEvent(ready));
    HANDLE events[] = {stopping, control, kick};
    for (;;) {
        DWORD event = WaitForMultipleObjects(3, events, FALSE, 30000);
        if (event == WAIT_OBJECT_0) break;
        if (event == WAIT_OBJECT_0 + 1) { operation(); CHECK(SetEvent(controlled)); }
        else if (event == WAIT_OBJECT_0 + 2) cpu_surface_snapshot_boundary(NULL);
        else { fprintf(stderr, "Owner event failed %lu\n", (unsigned long)event); exit(8); }
    }
    cpu_surface_shutdown(); return 0;
}
static void invoke(void (*function)(void)) {
    operation = function; CHECK(SetEvent(control));
    CHECK(WaitForSingleObject(controlled, 10000) == WAIT_OBJECT_0);
}
static void rejected(const VmReadSnapshotRange *ranges, u32 count) {
    uint8_t output[8]; memset(output, 0xa5, sizeof(output));
    CHECK(vm_read_snapshot(ranges, count, output, sizeof(output)) == VM_READ_SNAPSHOT_UNAVAILABLE);
    for (unsigned i = 0; i < sizeof(output); ++i) CHECK(output[i] == 0xa5);
}
static void query(void) {
    const VmReadSnapshotRange ranges[] = {{0x1000, 2, 0}, {0x1000 + (height - 1) * pitch + width - 2, 2, 2}};
    uint8_t output[8]; memset(output, 0xa5, sizeof(output));
    CHECK(vm_read_snapshot(ranges, 2, output, sizeof(output)) == VM_READ_SNAPSHOT_OK);
    CHECK(output[0] == blue(0, 0) && output[1] == blue(1, 0));
    CHECK(output[2] == blue(width - 2, height - 1) && output[3] == blue(width - 1, height - 1));
    for (unsigned i = 4; i < sizeof(output); ++i) CHECK(output[i] == 0xa5);
}
static DWORD WINAPI reader(void *unused) { (void)unused; for (int i = 0; i < 32; ++i) query(); return 0; }
static DWORD WINAPI retired_reader(void *unused) {
    (void)unused; const VmReadSnapshotRange range = {0x1000, 2, 0}; rejected(&range, 1); return 0;
}
static DWORD WINAPI release_gate(void *unused) {
    (void)unused; Sleep(100); return FAILED(gate->lpVtbl->Signal(gate, 1));
}
int main(int argc, char **argv) {
    _set_error_mode(_OUT_TO_STDERR); _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    CHECK(argc >= 2); int warp = !strcmp(argv[1], "warp");
    if (argc > 2) failure_kind = atoi(argv[2]);
    memset(memory, 0xcc, sizeof(memory));
    ID3D12Debug *debug = NULL;
    HR(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&debug));
    debug->lpVtbl->EnableDebugLayer(debug); debug->lpVtbl->Release(debug);
    IDXGIFactory4 *factory = NULL; IDXGIAdapter1 *adapter = NULL;
    HR(CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory));
    if (warp) HR(factory->lpVtbl->EnumWarpAdapter(factory, &IID_IDXGIAdapter1, (void **)&adapter));
    HR(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&s_d3d.device));
    if (!adapter) {
        LUID luid; s_d3d.device->lpVtbl->GetAdapterLuid(s_d3d.device, &luid);
        HR(factory->lpVtbl->EnumAdapterByLuid(factory, luid, &IID_IDXGIAdapter1, (void **)&adapter));
    }
    DXGI_ADAPTER_DESC1 description; HR(adapter->lpVtbl->GetDesc1(adapter, &description));
    CHECK(warp || !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE));
    wprintf(L"Adapter %ls software=%u\n", description.Description,
        !!(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)); fflush(stdout);
    D3D12_COMMAND_QUEUE_DESC queue = {0}; queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HR(s_d3d.device->lpVtbl->CreateCommandQueue(s_d3d.device, &queue, &IID_ID3D12CommandQueue, (void **)&s_d3d.cmd_queue));
    HR(s_d3d.device->lpVtbl->CreateFence(s_d3d.device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&gate));
    kick = CreateEventA(NULL, FALSE, FALSE, NULL); control = CreateEventA(NULL, FALSE, FALSE, NULL);
    controlled = CreateEventA(NULL, FALSE, FALSE, NULL); ready = CreateEventA(NULL, FALSE, FALSE, NULL);
    stopping = CreateEventA(NULL, FALSE, FALSE, NULL); CHECK(kick && control && controlled && ready && stopping);
    paused = CreateEventA(NULL, FALSE, FALSE, NULL); resume = CreateEventA(NULL, FALSE, FALSE, NULL);
    CHECK(paused && resume);
    HANDLE thread = CreateThread(NULL, 0, owner, NULL, 0, NULL); CHECK(thread);
    CHECK(WaitForSingleObject(ready, 10000) == WAIT_OBJECT_0);
    invoke(make_target);
    if (failure_kind) {
        invoke(produce); change_kind = 7; invoke(change); query();
        fprintf(stderr, "Expected production error was suppressed\n"); return 6;
    }
    change_kind = 6; invoke(change); invoke(produce);
    HANDLE unblock = CreateThread(NULL, 0, release_gate, NULL, 0, NULL); CHECK(unblock);
    query(); CHECK(gate->lpVtbl->GetCompletedValue(gate) >= 1);
    CHECK(WaitForSingleObject(unblock, 2000) == WAIT_OBJECT_0); DWORD exit_code = 1;
    CHECK(GetExitCodeThread(unblock, &exit_code) && !exit_code); CloseHandle(unblock);
    invoke(probe); CHECK(observed.gpu == observed.cpu && observed.fence == 2);
    u64 clean_fence = observed.fence; invoke(release_upload);
    query(); invoke(probe); CHECK(observed.fence == clean_fence);
    invoke(direct_rejection);
    HANDLE readers[4];
    for (int i = 0; i < 4; ++i) { readers[i] = CreateThread(NULL, 0, reader, NULL, 0, NULL); CHECK(readers[i]); }
    CHECK(WaitForMultipleObjects(4, readers, TRUE, 10000) == WAIT_OBJECT_0);
    for (int i = 0; i < 4; ++i) { CHECK(GetExitCodeThread(readers[i], &exit_code) && !exit_code); CloseHandle(readers[i]); }
    invoke(probe); CHECK(observed.fence == clean_fence);
    pattern = 2; invoke(produce);
    const VmReadSnapshotRange range = {0x1000, 2, 0};
    const int rejected_states[] = {1, 2, 3, 8};
    for (unsigned i = 0; i < sizeof(rejected_states) / sizeof(rejected_states[0]); ++i) {
        int kind = rejected_states[i];
        change_kind = kind; invoke(change); rejected(&range, 1);
        invoke(probe); CHECK(observed.fence == clean_fence);
        if (kind == 3) { change_kind = 4; invoke(change); }
        change_kind = 0; invoke(change);
    }
    query(); invoke(probe); CHECK(observed.fence == clean_fence + 2 && observed.gpu == observed.cpu);
    const VmReadSnapshotRange padding = {0x1000 + width - 1, 2, 0}; uint8_t padded[2] = {0};
    CHECK(vm_read_snapshot(&padding, 1, padded, 2) == VM_READ_SNAPSHOT_OK);
    CHECK(padded[0] == blue(width - 1, 0) && padded[1] == 0xcc);
    const VmReadSnapshotRange outside = {0x1000 + pitch * height, 1, 0}; rejected(&outside, 1);
    uint8_t retained[2]; CHECK(vm_read_snapshot(&range, 1, retained, 2) == VM_READ_SNAPSHOT_OK);
    uint8_t retained_first = blue(0, 0), retained_second = blue(1, 0);
    invoke(release_upload); pattern = 4; invoke(produce);
    operation = retire_pending; CHECK(SetEvent(control));
    CHECK(WaitForSingleObject(paused, 10000) == WAIT_OBJECT_0);
    for (int i = 0; i < 4; ++i) {
        readers[i] = CreateThread(NULL, 0, retired_reader, NULL, 0, NULL); CHECK(readers[i]);
    }
    unsigned pending = 0; ULONGLONG deadline = GetTickCount64() + 5000;
    while (pending != 4 && GetTickCount64() < deadline) {
        AcquireSRWLockShared(&s_cpu_surface_lock); pending = 0;
        for (CpuSurfaceRequest *request = s_cpu_surface_head; request; request = request->next) ++pending;
        ReleaseSRWLockShared(&s_cpu_surface_lock); if (pending != 4) Sleep(1);
    }
    CHECK(pending == 4); CHECK(SetEvent(resume));
    CHECK(WaitForSingleObject(controlled, 10000) == WAIT_OBJECT_0);
    CHECK(WaitForMultipleObjects(4, readers, TRUE, 10000) == WAIT_OBJECT_0);
    for (int i = 0; i < 4; ++i) {
        CHECK(GetExitCodeThread(readers[i], &exit_code) && !exit_code); CloseHandle(readers[i]);
    }
    rejected(&range, 1); CHECK(memory[0x1000] == blue(0, 0));
    CHECK(retained[0] == retained_first && retained[1] == retained_second);
    width = 8; height = 5; pitch = 12; pattern = 3;
    invoke(make_target); invoke(produce); query(); change_kind = 5; invoke(change);
    CHECK(SetEvent(stopping)); CHECK(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0); CloseHandle(thread);
    uint8_t unavailable = 0xa5;
    CHECK(vm_read_snapshot(&range, 1, &unavailable, 1) == VM_READ_SNAPSHOT_INVALID);
    CHECK(vm_read_snapshot(&outside, 1, &unavailable, 1) == VM_READ_SNAPSHOT_UNAVAILABLE && unavailable == 0xa5);
    ID3D12InfoQueue *info = NULL;
    HR(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device, &IID_ID3D12InfoQueue, (void **)&info));
    UINT64 messages = info->lpVtbl->GetNumStoredMessages(info);
    for (UINT64 i = 0; i < messages; ++i) {
        SIZE_T size = 0; HR(info->lpVtbl->GetMessage(info, i, NULL, &size));
        D3D12_MESSAGE *message = malloc(size); CHECK(message);
        HR(info->lpVtbl->GetMessage(info, i, message, &size));
        fprintf(stderr, "Validation %s\n", message->pDescription); free(message);
    }
    CHECK(!messages); info->lpVtbl->Release(info);
    CloseHandle(kick); CloseHandle(control); CloseHandle(controlled); CloseHandle(ready); CloseHandle(stopping);
    CloseHandle(paused); CloseHandle(resume);
    gate->lpVtbl->Release(gate); s_d3d.cmd_queue->lpVtbl->Release(s_d3d.cmd_queue);
    adapter->lpVtbl->Release(adapter); factory->lpVtbl->Release(factory); s_d3d.device->lpVtbl->Release(s_d3d.device);
    puts("PASS actual snapshot GPU producer: dirty/clean, blocked queue, padding, alias, retirement, resize, 128 concurrent reads, zero validation messages");
    return 0;
}
