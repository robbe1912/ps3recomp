/* ps3recomp - Address-keyed D24S8 attachments for the D3D12 backend. */
#ifndef PS3RECOMP_RSX_D3D12_DEPTH_H
#define PS3RECOMP_RSX_D3D12_DEPTH_H

#include <stdint.h>
#include <string.h>
#include <windows.h>
#include <d3d12.h>

#define RSX_D3D12_DEPTH_CAPACITY 128

typedef struct rsx_d3d12_depth_surface {
    ID3D12Resource* resource;
    uint32_t offset, width, height;
    int initialized;
} rsx_d3d12_depth_surface;

typedef struct rsx_d3d12_depth_cache {
    rsx_d3d12_depth_surface surfaces[RSX_D3D12_DEPTH_CAPACITY];
    uint32_t count;
} rsx_d3d12_depth_cache;

/* Slot zero belongs to the legacy attachment. Guest surfaces use slots 1..128. */
static D3D12_CPU_DESCRIPTOR_HANDLE rsx_d3d12_depth_handle(
    ID3D12DescriptorHeap* heap, uint32_t step, uint32_t slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(heap, &handle);
    handle.ptr += (SIZE_T)step * slot;
    return handle;
}

/* Call before recording commands, after all earlier GPU work completes.
 * The caller prepares every draw first. A later size request in that batch
 * can then grow a surface without discarding commands or changing its slot. */
static HRESULT rsx_d3d12_depth_prepare(rsx_d3d12_depth_cache* cache,
    ID3D12Device* device, ID3D12DescriptorHeap* heap, uint32_t step,
    uint32_t offset, uint32_t width, uint32_t height, uint32_t* out_slot)
{
    uint32_t index;
    rsx_d3d12_depth_surface* surface;
    D3D12_HEAP_PROPERTIES properties = {0};
    D3D12_RESOURCE_DESC description = {0};
    D3D12_DEPTH_STENCIL_VIEW_DESC view = {0};
    D3D12_CLEAR_VALUE clear = {0};
    ID3D12Resource* resource = NULL;
    D3D12_CPU_DESCRIPTOR_HANDLE handle;
    HRESULT result;
    if (!width || !height) return E_INVALIDARG;
    for (index = 0; index < cache->count; ++index)
        if (cache->surfaces[index].offset == offset) break;
    if (index == RSX_D3D12_DEPTH_CAPACITY) return E_OUTOFMEMORY;
    surface = &cache->surfaces[index];
    *out_slot = index + 1;
    if (surface->resource && surface->width >= width && surface->height >= height)
        return S_OK;
    if (surface->width > width) width = surface->width;
    if (surface->height > height) height = surface->height;
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    description.Width = width;
    description.Height = height;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    description.SampleDesc.Count = 1;
    description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    clear.Format = description.Format;
    clear.DepthStencil.Depth = 1.0f;
    result = device->lpVtbl->CreateCommittedResource(device, &properties,
        D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &clear, &IID_ID3D12Resource, (void**)&resource);
    if (FAILED(result)) return result;
    view.Format = description.Format;
    view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    handle = rsx_d3d12_depth_handle(heap, step, *out_slot);
    device->lpVtbl->CreateDepthStencilView(device, resource, &view, handle);
    if (surface->resource) surface->resource->lpVtbl->Release(surface->resource);
    surface->resource = resource;
    surface->offset = offset;
    surface->width = width;
    surface->height = height;
    surface->initialized = 0;
    if (index == cache->count) ++cache->count;
    return S_OK;
}

static void rsx_d3d12_depth_release(rsx_d3d12_depth_cache* cache)
{
    uint32_t index;
    for (index = 0; index < cache->count; ++index)
        cache->surfaces[index].resource->lpVtbl->Release(cache->surfaces[index].resource);
    memset(cache, 0, sizeof(*cache));
}

#endif
