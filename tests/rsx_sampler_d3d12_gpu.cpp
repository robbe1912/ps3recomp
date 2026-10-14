/* Authored D3D12 descriptor-table regression; no game input. */
#define WIN32_LEAN_AND_MEAN
#include "rsx_d3d12_sampler.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr, const char *what) {
  if (FAILED(hr)) {
    fprintf(stderr, "%s: %08X\n", what, (unsigned)hr);
    exit(2);
  }
}
static ComPtr<ID3DBlob> compile(const char *source, const char *profile) {
  ComPtr<ID3DBlob> code, error;
  HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr,
                          "main", profile, 0, 0, &code, &error);
  if (FAILED(hr) && error)
    fwrite(error->GetBufferPointer(), 1, error->GetBufferSize(), stderr);
  check(hr, "compile");
  return code;
}
int main(int argc, char **argv) {
  bool old = argc > 1 && !strcmp(argv[1], "--unpatched"),
       hardware = argc > 1 && !strcmp(argv[1], "--hardware");
  ComPtr<IDXGIFactory4> factory;
  check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
  ComPtr<IDXGIAdapter1> adapter;
  if (hardware)
    check(factory->EnumAdapters1(0, &adapter), "hardware adapter");
  else
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP");
  DXGI_ADAPTER_DESC1 identity = {};
  check(adapter->GetDesc1(&identity), "adapter identity");
  if (hardware && (identity.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
    fprintf(stderr, "hardware requested but software adapter selected\n");
    return 2;
  }
  printf("device=%s vendor=%04X device=%04X\n", hardware ? "hardware" : "WARP",
         identity.VendorId, identity.DeviceId);
  ComPtr<ID3D12Device> device;
  check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                          IID_PPV_ARGS(&device)),
        "device");
  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC q = {};
  check(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
  ComPtr<ID3D12CommandAllocator> alloc;
  check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       IID_PPV_ARGS(&alloc)),
        "allocator");
  ComPtr<ID3D12GraphicsCommandList> list;
  check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                  alloc.Get(), nullptr, IID_PPV_ARGS(&list)),
        "list");
  ComPtr<ID3D12Fence> fence;
  check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
        "fence");
  HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event)
    return 2;
  UINT64 serial = 0;
  auto wait = [&]() {
    check(queue->Signal(fence.Get(), ++serial), "signal");
    check(fence->SetEventOnCompletion(serial, event), "fence event");
    if (WaitForSingleObject(event, 15000) != WAIT_OBJECT_0)
      exit(2);
  };
  auto heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type, UINT n, bool visible) {
    ComPtr<ID3D12DescriptorHeap> h;
    D3D12_DESCRIPTOR_HEAP_DESC d = {};
    d.Type = type;
    d.NumDescriptors = n;
    d.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                      : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    check(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&h)), "heap");
    return h;
  };
  auto srvheap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, true),
       samplers = heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 112, true),
       rtvheap = heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false);
  auto resource = [&](D3D12_RESOURCE_DESC desc, D3D12_HEAP_TYPE type,
                      D3D12_RESOURCE_STATES state) {
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES p = {};
    p.Type = type;
    check(device->CreateCommittedResource(&p, D3D12_HEAP_FLAG_NONE, &desc,
                                          state, nullptr, IID_PPV_ARGS(&r)),
          "resource");
    return r;
  };
  D3D12_RESOURCE_DESC td = {};
  td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width = td.Height = 2;
  td.DepthOrArraySize = td.MipLevels = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  auto texture =
      resource(td, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
  td.Width = td.Height = 1;
  td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  auto output =
      resource(td, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_SOURCE);
  device->CreateRenderTargetView(output.Get(), nullptr,
                                 rtvheap->GetCPUDescriptorHandleForHeapStart());
  D3D12_RESOURCE_DESC bd = {};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = 512;
  bd.Height = bd.DepthOrArraySize = bd.MipLevels = bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  auto upload = resource(bd, D3D12_HEAP_TYPE_UPLOAD,
                         D3D12_RESOURCE_STATE_GENERIC_READ),
       readback = resource(bd, D3D12_HEAP_TYPE_READBACK,
                           D3D12_RESOURCE_STATE_COPY_DEST);
  unsigned char bytes[] = {255, 0, 0,   255, 0,   255, 0,   255,
                           0,   0, 255, 255, 255, 255, 255, 255};
  void *mapped = nullptr;
  check(upload->Map(0, nullptr, &mapped), "upload map");
  memcpy(mapped, bytes, 8);
  memcpy((char *)mapped + 256, bytes + 8, 8);
  upload->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
  src.pResource = upload.Get();
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, 1, 256};
  dst.pResource = texture.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  auto barrier = [&](ID3D12Resource *r, D3D12_RESOURCE_STATES before,
                     D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
  };
  barrier(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
  sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  sv.Texture2D.MipLevels = 1;
  auto cpu = srvheap->GetCPUDescriptorHandleForHeapStart();
  UINT si = device->GetDescriptorHandleIncrementSize(
           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
       sa = device->GetDescriptorHandleIncrementSize(
           D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  for (unsigned u = 0; u < 16; ++u) {
    device->CreateShaderResourceView(texture.Get(), &sv, cpu);
    cpu.ptr += si;
  }
  D3D12_DESCRIPTOR_RANGE ranges[2] = {};
  ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
  for (auto &r : ranges)
    r.NumDescriptors = 16;
  D3D12_ROOT_PARAMETER params[4] = {};
  params[0].ParameterType = params[2].ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_CBV;
  params[0].Descriptor.ShaderRegister = 0;
  params[2].Descriptor.ShaderRegister = 1;
  for (unsigned i = 0; i < 2; ++i) {
    auto &p = params[1 + i * 2];
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable = {1, &ranges[i]};
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }
  D3D12_ROOT_SIGNATURE_DESC rd = {};
  rd.NumParameters = 4;
  rd.pParameters = params;
  ComPtr<ID3DBlob> rootblob, error;
  check(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1,
                                    &rootblob, &error),
        "root serialize");
  ComPtr<ID3D12RootSignature> root;
  check(device->CreateRootSignature(0, rootblob->GetBufferPointer(),
                                    rootblob->GetBufferSize(),
                                    IID_PPV_ARGS(&root)),
        "root");
  auto vs = compile("float4 main(uint i:SV_VertexID):SV_Position{return "
                    "float4(i==1?3:-1,i==2?3:-1,0,1);}",
                    "vs_5_0");
  struct Case {
    unsigned unit, address, filter;
    float u, v, want[4];
  } cases[] = {{0, 0x10101, 0x01010000, 1.25f, .25f, {1, 0, 0, 1}},
               {0, 0x10101, 0x02020000, .5f, .5f, {.5f, .5f, .5f, 1}},
               {0, 0x30303, 0x01010000, 1.25f, .25f, {0, 1, 0, 1}},
               {0, 0x30301, 0x01010000, 1.25f, 1.25f, {0, 0, 1, 1}},
               {7, 0x30303, 0x01010000, 1.25f, .25f, {0, 1, 0, 1}},
               {0, 0x40404, 0x01010000, 2, 2, {0, 0, 0, 0}},
               {0, 0x20202, 0x01010000, 1.25f, .25f, {0, 1, 0, 1}}};
  // All tables are populated BEFORE any submission: accidental descriptor reuse
  // changes earlier draws.
  cpu = samplers->GetCPUDescriptorHandleForHeapStart();
  for (const auto &c : cases)
    for (unsigned u = 0; u < 16; ++u) {
      D3D12_SAMPLER_DESC d;
      if (!rsx_d3d12_sampler(u == c.unit ? c.address : 0x30303,
                             u == c.unit ? c.filter : 0x01010000, 0, 0, &d))
        return 3;
      if (old) {
        d.Filter = u == 0 ? D3D12_FILTER_MIN_MAG_MIP_POINT
                          : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        d.AddressU = d.AddressV = d.AddressW =
            u == 0 ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
                   : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
      }
      device->CreateSampler(&d, cpu);
      cpu.ptr += sa;
    }
  int failed = 0;
  for (unsigned i = 0; i < 7; ++i) {
    const auto &c = cases[i];
    char hlsl[600];
    snprintf(hlsl, sizeof hlsl,
             "Texture2D tex:register(t%u);SamplerState "
             "rsx_samp[16]:register(s0);float4 main(float4 "
             "p:SV_Position):SV_Target{return "
             "tex.SampleLevel(rsx_samp[%u],float2(%.9g,%.9g),0);}",
             c.unit, c.unit, (double)c.u, (double)c.v);
    if (rsx_hlsl_sampler_mask(hlsl) != (1u << c.unit))
      return 4;
    auto ps = compile(hlsl, "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = root.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 15;
    pd.SampleMask = ~0u;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
    pd.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    check(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)), "PSO");
    barrier(output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto rt = rtvheap->GetCPUDescriptorHandleForHeapStart();
    list->OMSetRenderTargets(1, &rt, FALSE, nullptr);
    D3D12_VIEWPORT vp = {0, 0, 1, 1, 0, 1};
    D3D12_RECT rect = {0, 0, 1, 1};
    list->RSSetViewports(1, &vp);
    list->RSSetScissorRects(1, &rect);
    list->SetGraphicsRootSignature(root.Get());
    list->SetPipelineState(pso.Get());
    ID3D12DescriptorHeap *heaps[] = {srvheap.Get(), samplers.Get()};
    list->SetDescriptorHeaps(2, heaps);
    list->SetGraphicsRootDescriptorTable(
        1, srvheap->GetGPUDescriptorHandleForHeapStart());
    auto gh = samplers->GetGPUDescriptorHandleForHeapStart();
    gh.ptr += (UINT64)i * 16 * sa;
    list->SetGraphicsRootDescriptorTable(3, gh);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    barrier(output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
    src = {};
    dst = {};
    src.pResource = output.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, 1,
                                     256};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    check(list->Close(), "close");
    ID3D12CommandList *lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    wait();
    check(readback->Map(0, nullptr, &mapped), "read map");
    float got[4];
    memcpy(got, mapped, sizeof got);
    readback->Unmap(0, nullptr);
    bool ok = true;
    for (unsigned k = 0; k < 4; ++k)
      if (!std::isfinite(got[k]) || fabs(got[k] - c.want[k]) > 1e-5f)
        ok = false;
    printf("case %u %.6f %.6f %.6f %.6f %s\n", i, got[0], got[1], got[2],
           got[3], ok ? "PASS" : "FAIL");
    failed += !ok;
    check(alloc->Reset(), "reset allocator");
    check(list->Reset(alloc.Get(), nullptr), "reset list");
  }
  CloseHandle(event);
  D3D12_SAMPLER_DESC d;
  if (!rsx_d3d12_sampler(0x10101, 0x02043f80, 0x80078010, 0, &d) ||
      d.Filter != D3D12_FILTER_ANISOTROPIC || d.MaxAnisotropy != 2 ||
      d.MipLODBias != -.5f || d.MaxLOD != 15)
    return 4;
  if (rsx_d3d12_sampler(0x30303, 0x04074000, 0, 0, &d))
    return 5;
  if (rsx_d3d12_sampler(0x50505, 0x01010000, 0, 0, &d) ||
      rsx_d3d12_sampler(0x30303, 0, 0, 0, &d))
    return 6;
  return failed ? 1 : 0;
}
