/* Game-free pixel checks for address-keyed D3D12 depth attachments. */
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include "rsx_d3d12_depth.h"
#include "rsx_commands.h"

static ID3D12Device* device;
static ID3D12CommandQueue* queue;
static ID3D12CommandAllocator* allocator;
static ID3D12GraphicsCommandList* list;
static ID3D12Fence* fence;
static ID3D12DescriptorHeap *rtv_heap, *dsv_heap;
static ID3D12Resource* targets[3];
static ID3D12Resource* readback;
static ID3D12RootSignature* root;
static ID3D12PipelineState *pipeline, *stencil_pipeline;
static ID3D12InfoQueue* messages;
static rsx_d3d12_depth_cache depths;
static UINT rtv_step, dsv_step;
static UINT64 fence_value;
static HANDLE event_handle;
static UINT slots[2];
static int legacy, last_target = -1;
static unsigned checks, failures;

static void check(HRESULT result, const char* operation)
{
    if (FAILED(result)) {
        fprintf(stderr, "%s HRESULT=0x%08lX\n", operation, (unsigned long)result);
        exit(2);
    }
}

static D3D12_CPU_DESCRIPTOR_HANDLE rtv(UINT index)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtv_heap, &handle);
    handle.ptr += (SIZE_T)index * rtv_step;
    return handle;
}

static D3D12_CPU_DESCRIPTOR_HANDLE depth(UINT index)
{
    return rsx_d3d12_depth_handle(dsv_heap, dsv_step, slots[legacy ? 0 : index]);
}

static void submit(void)
{
    ID3D12CommandList* commands[] = {(ID3D12CommandList*)list};
    check(list->lpVtbl->Close(list), "close");
    queue->lpVtbl->ExecuteCommandLists(queue, 1, commands);
    check(queue->lpVtbl->Signal(queue, fence, ++fence_value), "signal");
    if (fence->lpVtbl->GetCompletedValue(fence) < fence_value) {
        check(fence->lpVtbl->SetEventOnCompletion(fence, fence_value, event_handle), "fence event");
        if (WaitForSingleObject(event_handle, 30000) != WAIT_OBJECT_0) exit(3);
    }
    check(device->lpVtbl->GetDeviceRemovedReason(device), "device status");
    check(allocator->lpVtbl->Reset(allocator), "allocator reset");
    check(list->lpVtbl->Reset(list, allocator, NULL), "list reset");
}

static void clear_depth(UINT index, D3D12_CLEAR_FLAGS flags, float value, UINT8 stencil)
{
    list->lpVtbl->ClearDepthStencilView(list, depth(index), flags, value, stencil, 0, NULL);
}

void vm_write32(uint32_t address, uint32_t value)
{
    (void)address;
    (void)value;
    abort();
}

void ps3_ms(const char* key)
{
    (void)key;
    abort();
}
uint32_t ppu_hle_inject_base = 0;

static void command_clear(void* userdata, u32 flags, u32 color, float value, u8 stencil)
{
    (void)userdata;
    (void)color;
    D3D12_CLEAR_FLAGS selected = (D3D12_CLEAR_FLAGS)
        (((flags & 1) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
         ((flags & 2) ? D3D12_CLEAR_FLAG_STENCIL : 0));
    if (selected) clear_depth(0, selected, value, stencil);
}

static void draw(UINT target, UINT zeta, float red, float green, float blue, float z, int stencil)
{
    D3D12_CPU_DESCRIPTOR_HANDLE color = rtv(target), zview = depth(zeta);
    D3D12_VIEWPORT viewport = {0, 0, 32, 32, 0, 1};
    D3D12_RECT scissor = {0, 0, 32, 32};
    float constants[4] = {red, green, blue, z};
    if (legacy && last_target != (int)target)
        clear_depth(zeta, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1, 0);
    last_target = (int)target;
    list->lpVtbl->SetPipelineState(list, stencil ? stencil_pipeline : pipeline);
    list->lpVtbl->SetGraphicsRootSignature(list, root);
    list->lpVtbl->SetGraphicsRoot32BitConstants(list, 0, 4, constants, 0);
    list->lpVtbl->OMSetRenderTargets(list, 1, &color, FALSE, &zview);
    list->lpVtbl->OMSetStencilRef(list, 7);
    list->lpVtbl->RSSetViewports(list, 1, &viewport);
    list->lpVtbl->RSSetScissorRects(list, 1, &scissor);
    list->lpVtbl->IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->lpVtbl->DrawInstanced(list, 3, 1, 0, 0);
}

static void pixel(UINT target, UINT32 expected, const char* name)
{
    D3D12_RESOURCE_BARRIER barrier = {0};
    D3D12_TEXTURE_COPY_LOCATION source = {0}, destination = {0};
    D3D12_RANGE range = {0, 32 * 256};
    void* data = NULL;
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = targets[target];
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->lpVtbl->ResourceBarrier(list, 1, &barrier);
    source.pResource = targets[target];
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.pResource = readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    destination.PlacedFootprint.Footprint.Width = 32;
    destination.PlacedFootprint.Footprint.Height = 32;
    destination.PlacedFootprint.Footprint.Depth = 1;
    destination.PlacedFootprint.Footprint.RowPitch = 256;
    list->lpVtbl->CopyTextureRegion(list, &destination, 0, 0, 0, &source, NULL);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->lpVtbl->ResourceBarrier(list, 1, &barrier);
    submit();
    check(readback->lpVtbl->Map(readback, 0, &range, &data), "readback map");
    UINT32 actual;
    memcpy(&actual, (char*)data + 16 * 256 + 16 * 4, sizeof actual);
    readback->lpVtbl->Unmap(readback, 0, NULL);
    ++checks;
    failures += actual != expected;
    printf("%s %s pixel=%08X expected=%08X\n", actual == expected ? "PASS" : "FAIL",
        name, actual, expected);
}

int main(int argc, char** argv)
{
    int hardware = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--hardware")) hardware = 1;
        else if (!strcmp(argv[i], "--legacy")) legacy = 1;
        else return 2;
    }
    ID3D12Debug* debug = NULL;
    if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&debug))) {
        debug->lpVtbl->EnableDebugLayer(debug);
        debug->lpVtbl->Release(debug);
    } else printf("Debug layer unavailable\n");
    IDXGIFactory4* factory = NULL;
    IUnknown* adapter = NULL;
    check(CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory), "factory");
    if (!hardware) check(factory->lpVtbl->EnumWarpAdapter(factory, &IID_IUnknown,
        (void**)&adapter), "WARP adapter");
    check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
        (void**)&device), "device");
    if (adapter) adapter->lpVtbl->Release(adapter);
    factory->lpVtbl->Release(factory);
    device->lpVtbl->QueryInterface(device, &IID_ID3D12InfoQueue, (void**)&messages);
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    check(device->lpVtbl->CreateCommandQueue(device, &queue_desc, &IID_ID3D12CommandQueue,
        (void**)&queue), "queue");
    check(device->lpVtbl->CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator, (void**)&allocator), "allocator");
    check(device->lpVtbl->CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator, NULL, &IID_ID3D12GraphicsCommandList, (void**)&list), "list");
    check(device->lpVtbl->CreateFence(device, 0, D3D12_FENCE_FLAG_NONE,
        &IID_ID3D12Fence, (void**)&fence), "fence");
    event_handle = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!event_handle) return 2;
    D3D12_DESCRIPTOR_HEAP_DESC heap = {0};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = 3;
    check(device->lpVtbl->CreateDescriptorHeap(device, &heap, &IID_ID3D12DescriptorHeap,
        (void**)&rtv_heap), "RTV heap");
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap.NumDescriptors = 1 + RSX_D3D12_DEPTH_CAPACITY;
    check(device->lpVtbl->CreateDescriptorHeap(device, &heap, &IID_ID3D12DescriptorHeap,
        (void**)&dsv_heap), "DSV heap");
    rtv_step = device->lpVtbl->GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dsv_step = device->lpVtbl->GetDescriptorHandleIncrementSize(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    check(rsx_d3d12_depth_prepare(&depths, device, dsv_heap, dsv_step, 0x1000, 32, 32, &slots[0]), "depth A");
    check(rsx_d3d12_depth_prepare(&depths, device, dsv_heap, dsv_step, 0x2000, 32, 32, &slots[1]), "depth B");
    D3D12_HEAP_PROPERTIES properties = {0};
    D3D12_RESOURCE_DESC resource = {0};
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = resource.Height = 32;
    resource.DepthOrArraySize = resource.MipLevels = 1;
    resource.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    resource.SampleDesc.Count = 1;
    resource.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    float black[4] = {0, 0, 0, 1};
    for (UINT i = 0; i < 3; ++i) {
        check(device->lpVtbl->CreateCommittedResource(device, &properties, D3D12_HEAP_FLAG_NONE,
            &resource, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource,
            (void**)&targets[i]), "color target");
        device->lpVtbl->CreateRenderTargetView(device, targets[i], NULL, rtv(i));
        list->lpVtbl->ClearRenderTargetView(list, rtv(i), black, 0, NULL);
    }
    properties.Type = D3D12_HEAP_TYPE_READBACK;
    memset(&resource, 0, sizeof resource);
    resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource.Width = 32 * 256;
    resource.Height = resource.DepthOrArraySize = resource.MipLevels = 1;
    resource.SampleDesc.Count = 1;
    resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device->lpVtbl->CreateCommittedResource(device, &properties, D3D12_HEAP_FLAG_NONE,
        &resource, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
        (void**)&readback), "readback buffer");
    D3D12_ROOT_PARAMETER parameter = {0};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.Num32BitValues = 4;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {0};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &parameter;
    ID3DBlob *serialized = NULL, *error = NULL, *vs = NULL, *ps = NULL;
    check(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized, &error), "serialize root");
    check(device->lpVtbl->CreateRootSignature(device, 0,
        serialized->lpVtbl->GetBufferPointer(serialized), serialized->lpVtbl->GetBufferSize(serialized),
        &IID_ID3D12RootSignature, (void**)&root), "root");
    const char shader[] = "cbuffer C:register(b0){float4 p;}"
        "float4 vs(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,p.w,1);}"
        "float4 ps():SV_Target{return float4(p.xyz,1);}";
    check(D3DCompile(shader, sizeof shader - 1, NULL, NULL, NULL, "vs", "vs_5_0", 0, 0, &vs, &error), "vertex shader");
    check(D3DCompile(shader, sizeof shader - 1, NULL, NULL, NULL, "ps", "ps_5_0", 0, 0, &ps, &error), "pixel shader");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC state = {0};
    state.pRootSignature = root;
    state.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
    state.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
    state.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
    state.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
    state.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    state.RasterizerState.DepthClipEnable = TRUE;
    state.DepthStencilState.DepthEnable = TRUE;
    state.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    state.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    state.DepthStencilState.StencilReadMask = state.DepthStencilState.StencilWriteMask = 255;
    state.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    state.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    state.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    state.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
    state.DepthStencilState.BackFace = state.DepthStencilState.FrontFace;
    state.SampleMask = UINT_MAX;
    state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    state.NumRenderTargets = 1;
    state.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    state.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    state.SampleDesc.Count = 1;
    check(device->lpVtbl->CreateGraphicsPipelineState(device, &state,
        &IID_ID3D12PipelineState, (void**)&pipeline), "depth pipeline");
    state.DepthStencilState.DepthEnable = FALSE;
    state.DepthStencilState.StencilEnable = TRUE;
    check(device->lpVtbl->CreateGraphicsPipelineState(device, &state,
        &IID_ID3D12PipelineState, (void**)&stencil_pipeline), "stencil pipeline");
    clear_depth(0, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1, 7);
    clear_depth(1, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1, 0);
    draw(0, 0, 1, 0, 0, .2f, 0);
    draw(1, 0, 0, 1, 0, .8f, 0);
    pixel(1, 0xFF000000, "same-zeta color switch preserves occlusion");
    check(rsx_d3d12_depth_prepare(&depths, device, dsv_heap, dsv_step, 0x1000, 16, 16, &slots[0]), "depth A smaller clip");
    draw(2, 1, 0, 1, 0, .8f, 0);
    pixel(2, 0xFF00FF00, "other-zeta draw has independent depth");
    draw(0, 0, 0, 0, 1, .6f, 0);
    pixel(0, 0xFF0000FF, "smaller clip and scene return retain foreground depth");
    clear_depth(1, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, .9f, 0);
    draw(0, 0, 0, 0, 1, .6f, 0);
    pixel(0, 0xFF0000FF, "other-zeta clear preserves scene depth");
    draw(0, 0, 1, 1, 0, 0, 1);
    pixel(0, 0xFF00FFFF, "other-zeta clear preserves scene stencil");
    clear_depth(0, D3D12_CLEAR_FLAG_DEPTH, .9f, 0);
    draw(0, 0, 0, 0, 1, .6f, 0);
    pixel(0, 0xFFFF0000, "selected-zeta explicit clear permits rear draw");
    rsx_state guest = {0};
    rsx_backend backend = {0};
    backend.clear = command_clear;
    rsx_set_backend(&backend);
    D3D12_CPU_DESCRIPTOR_HANDLE prior_target = rtv(0);
    const float prior_color[4] = {1, 0, 1, 1};
    list->lpVtbl->ClearRenderTargetView(list, prior_target, prior_color, 0, NULL);
    rsx_process_method(&guest, NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0xFFFFFF07);
    rsx_process_method(&guest, NV4097_CLEAR_SURFACE, 3);
    draw(0, 0, 0, 0, 1, 1, 0);
    pixel(0, 0xFFFF0000, "packed far clear permits sky to replace prior-frame color");
    draw(0, 0, 0, 1, 0, 0, 1);
    pixel(0, 0xFF00FF00, "packed clear selects the low-byte stencil");
    rsx_process_method(&guest, NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0x80000007);
    rsx_process_method(&guest, NV4097_CLEAR_SURFACE, 1);
    draw(0, 0, 1, 0, 0, .6f, 0);
    pixel(0, 0xFF00FF00, "packed half-depth clear rejects a rear fragment");
    draw(0, 0, 1, 0, 0, .4f, 0);
    pixel(0, 0xFF0000FF, "packed half-depth clear permits a near fragment");
    rsx_process_method(&guest, NV4097_SET_ZSTENCIL_CLEAR_VALUE, 0x00000007);
    rsx_process_method(&guest, NV4097_CLEAR_SURFACE, 1);
    draw(0, 0, 0, 0, 1, .1f, 0);
    pixel(0, 0xFF0000FF, "packed zero-depth clear rejects a positive fragment");
    draw(0, 0, 0, 0, 1, 0, 1);
    pixel(0, 0xFFFF0000, "depth-only clears preserve the decoded stencil");
    rsx_set_backend(NULL);
    if (messages) {
        UINT64 count = messages->lpVtbl->GetNumStoredMessages(messages);
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0;
            messages->lpVtbl->GetMessage(messages, i, NULL, &size);
            D3D12_MESSAGE* message = malloc(size);
            check(messages->lpVtbl->GetMessage(messages, i, message, &size), "validation message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                fprintf(stderr, "GPU validation: %s\n", message->pDescription);
                ++failures;
            }
            free(message);
        }
    }
    rsx_d3d12_depth_release(&depths);
    CloseHandle(event_handle);
    printf("%s %s checks=%u failures=%u\n", hardware ? "hardware" : "warp",
        legacy ? "legacy-control" : "address-keyed", checks, failures);
    return failures ? 1 : 0;
}
