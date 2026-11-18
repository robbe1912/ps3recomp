/* Authored independent MRT write-mask regression; no game input. */
#define WIN32_LEAN_AND_MEAN
#include "rsx_d3d12_mrt.h"
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
  bool old=false,hardware=false,blend_policy=false;
  for(int i=1;i<argc;i++) {
    if(!strcmp(argv[i],"--unpatched"))old=true;
    else if(!strcmp(argv[i],"--hardware"))hardware=true;
    else if(!strcmp(argv[i],"--blend-policy"))blend_policy=true;
    else return 2;
  }
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

  auto rtvheap=heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV,4,false);
  UINT stride=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  auto resource=[&](D3D12_RESOURCE_DESC d,D3D12_HEAP_TYPE t,D3D12_RESOURCE_STATES s){
    D3D12_HEAP_PROPERTIES hp={}; hp.Type=t;ComPtr<ID3D12Resource> r;
    check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,s,nullptr,IID_PPV_ARGS(&r)),"resource");return r;
  };
  D3D12_RESOURCE_DESC td={};td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  td.Width=td.Height=td.DepthOrArraySize=td.MipLevels=td.SampleDesc.Count=1;
  td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  ComPtr<ID3D12Resource> targets[4];D3D12_CPU_DESCRIPTOR_HANDLE rt[4];
  for(unsigned r=0;r<4;r++){
    targets[r]=resource(td,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    rt[r]=rtvheap->GetCPUDescriptorHandleForHeapStart();rt[r].ptr+=r*stride;
    device->CreateRenderTargetView(targets[r].Get(),nullptr,rt[r]);
  }
  constexpr UINT64 target_stride = D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  D3D12_RESOURCE_DESC bd={};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=4*target_stride;
  bd.Height=bd.DepthOrArraySize=bd.MipLevels=bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  auto readback=resource(bd,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  auto barrier=[&](unsigned r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){
    D3D12_RESOURCE_BARRIER t={};t.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition={targets[r].Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};list->ResourceBarrier(1,&t);
  };
  D3D12_ROOT_SIGNATURE_DESC rd={};ComPtr<ID3DBlob> blob,error;
  check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"root serialize");
  ComPtr<ID3D12RootSignature> root;
  check(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"root");
  auto vs=compile("float4 main(uint i:SV_VertexID):SV_Position{return float4(i==1?3:-1,i==2?3:-1,0,1);}","vs_5_0");
  auto ps=compile("struct O{float4 a:SV_Target0;float4 b:SV_Target1;float4 c:SV_Target2;float4 d:SV_Target3;};O main(float4 p:SV_Position){O o;o.a=o.b=o.c=o.d=float4(.75,.5,.25,0);return o;}","ps_5_0");
  struct Case{unsigned masks,nrt;};Case cases[]={{0x000f,2},{0x00ff,2},{0x001f,2},{0x008f,2},{0x00f0,2},{0x8421,4},{0,4}};
  const float clear[]={.125f,.25f,.5f,1},draw[]={.75f,.5f,.25f,0};int failed=0;
  for(unsigned i=0;i<7;i++){
    auto c=cases[i];const unsigned enable_cases[]={1,2,3,5,10,15,0};
    unsigned enables=enable_cases[i];if(blend_policy){c.masks=0xFFFF;c.nrt=4;}D3D12_GRAPHICS_PIPELINE_STATE_DESC pd={};
    pd.pRootSignature=root.Get();pd.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pd.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    pd.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pd.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pd.RasterizerState.DepthClipEnable=TRUE;
    pd.SampleMask=~0u;pd.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pd.NumRenderTargets=c.nrt;pd.SampleDesc.Count=1;
    for(unsigned r=0;r<c.nrt;r++)pd.RTVFormats[r]=DXGI_FORMAT_R32G32B32A32_FLOAT;
    rsx_d3d12_write_masks(&pd.BlendState,c.nrt,c.masks);
    if(blend_policy){
      for(unsigned r=0;r<c.nrt;r++){
        auto& b=pd.BlendState.RenderTarget[r];b.SrcBlend=b.DestBlend=b.SrcBlendAlpha=b.DestBlendAlpha=D3D12_BLEND_ONE;
        b.BlendOp=b.BlendOpAlpha=D3D12_BLEND_OP_ADD;
      }
      rsx_d3d12_blend_enables(&pd.BlendState,c.nrt,enables);
      if(old)for(unsigned r=0;r<c.nrt;r++)pd.BlendState.RenderTarget[r].BlendEnable=(enables&1)?TRUE:FALSE;
    }
    if(old&&!blend_policy){pd.BlendState.IndependentBlendEnable=FALSE;for(unsigned r=0;r<c.nrt;r++)pd.BlendState.RenderTarget[r].RenderTargetWriteMask=(UINT8)(c.masks&15);}
    ComPtr<ID3D12PipelineState> pso;check(device->CreateGraphicsPipelineState(&pd,IID_PPV_ARGS(&pso)),"PSO");
    for(unsigned r=0;r<4;r++)list->ClearRenderTargetView(rt[r],clear,0,nullptr);
    list->OMSetRenderTargets(c.nrt,rt,FALSE,nullptr);D3D12_VIEWPORT vp={0,0,1,1,0,1};D3D12_RECT rect={0,0,1,1};
    list->RSSetViewports(1,&vp);list->RSSetScissorRects(1,&rect);list->SetGraphicsRootSignature(root.Get());list->SetPipelineState(pso.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);
    for(unsigned r=0;r<c.nrt;r++){
      barrier(r,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION src={},dst={};src.pResource=targets[r].Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint.Offset=r*target_stride;
      dst.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,1,1,1,D3D12_TEXTURE_DATA_PITCH_ALIGNMENT};list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
      barrier(r,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    check(list->Close(),"close");ID3D12CommandList* commands[]={list.Get()};queue->ExecuteCommandLists(1,commands);wait();
    void* mapped=nullptr;check(readback->Map(0,nullptr,&mapped),"readback");bool ok=true;
    for(unsigned r=0;r<c.nrt;r++){
      float got[4];memcpy(got,(char*)mapped+r*target_stride,sizeof got);
      for(unsigned k=0;k<4;k++){
        float want=blend_policy ? draw[k]+((enables&(1u<<r))?clear[k]:0) : ((c.masks&(1u<<(r*4+k)))?draw[k]:clear[k]);
        if(!std::isfinite(got[k])||fabs(got[k]-want)>1e-5){ok=false;printf("target %u channel %u got %.6f want %.6f\n",r,k,got[k],want);}
      }
    }
    readback->Unmap(0,nullptr);printf("case %u masks=%04X nrt=%u %s\n",i,c.masks,c.nrt,ok?"PASS":"FAIL");failed+=!ok;
    check(alloc->Reset(),"reset allocator");check(list->Reset(alloc.Get(),nullptr),"reset list");
  }
  CloseHandle(event);return failed?1:0;
}
