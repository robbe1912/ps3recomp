/* The FACING input (0xE) through the production decompiler: +1 on front faces,
 * -1 on back faces, under both winding conventions. */
#define WIN32_LEAN_AND_MEAN
#include "rsx_fp_decompiler.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <string>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
static void checked(HRESULT hr, const char *what) {
    if (FAILED(hr)) {
        fprintf(stderr, "%s: %08X\n", what, (unsigned)hr);
        exit(2);
    }
}
static ComPtr<ID3DBlob> compile(const std::string &text, const char *target) {
    ComPtr<ID3DBlob> code, error;
    HRESULT hr = D3DCompile(text.data(), text.size(), nullptr, nullptr, nullptr, "main", target, 0,
                            0, &code, &error);
    if (FAILED(hr) && error)
        fwrite(error->GetBufferPointer(), 1, error->GetBufferSize(), stderr);
    checked(hr, "compile");
    return code;
}
static void word(u8 *p, u32 v) {
    v = (v << 16) | (v >> 16);
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}
/* MOV R0, <input>; END. */
static std::string program(u32 input) {
    u8 code[16] = {};
    const u32 swizzle = 0xE4u << 9, exec = 7u << 18;
    word(code, (1u << 24) | (15u << 9) | (input << 13) | 1u);
    word(code + 4, swizzle | exec | 1u);
    static char hlsl[65536];
    if (rsx_fp_decompile(code, sizeof code, 0x40, hlsl, sizeof hlsl) != 1) {
        fprintf(stderr, "decompile failed\n");
        exit(2);
    }
    return hlsl;
}
int main(int argc, char **argv) {
    bool hardware = false;
    for (int i = 1; i < argc; ++i)
        hardware |= !strcmp(argv[i], "--hardware");
    unsigned checks = 0, failures = 0;
    /* A program that does not read FACING keeps its old input signature. */
    const std::string plain = program(0x4);
    const bool plain_ok = plain.find("SV_IsFrontFace") == std::string::npos;
    ++checks;
    failures += !plain_ok;
    printf("%s TC0 program declares no SV_IsFrontFace\n", plain_ok ? "PASS" : "FAIL");
    const std::string facing = program(0xE);
    const bool decl_ok = facing.find("SV_IsFrontFace") != std::string::npos;
    ++checks;
    failures += !decl_ok;
    printf("%s FACING program declares SV_IsFrontFace\n", decl_ok ? "PASS" : "FAIL");
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level;
    checked(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP,
                              nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &ctx),
            "device");
    /* Clockwise in NDC (y up): D3D's default front face. */
    auto vs_code = compile("struct O { float4 p:SV_POSITION; }; O main(uint i:SV_VertexID) { O o; "
                           "o.p=float4(i==2?3:-1,i==1?3:-1,0,1);return o;}",
                           "vs_5_0");
    ComPtr<ID3D11VertexShader> vs;
    checked(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(),
                                       nullptr, &vs),
            "VS");
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> rt, readback;
    checked(device->CreateTexture2D(&td, nullptr, &rt), "target");
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    checked(device->CreateTexture2D(&td, nullptr, &readback), "readback");
    ComPtr<ID3D11RenderTargetView> rtv;
    checked(device->CreateRenderTargetView(rt.Get(), nullptr, &rtv), "RTV");
    auto ps_code = compile(facing, "ps_5_0");
    ComPtr<ID3D11PixelShader> ps;
    checked(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(),
                                      nullptr, &ps),
            "PS");
    D3D11_VIEWPORT vp = {0, 0, 1, 1, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs.Get(), nullptr, 0);
    ctx->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11RenderTargetView *target = rtv.Get();
    ctx->OMSetRenderTargets(1, &target, nullptr);
    for (int ccw = 0; ccw < 2; ++ccw) {
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        rd.FrontCounterClockwise = ccw ? TRUE : FALSE;
        ComPtr<ID3D11RasterizerState> raster;
        checked(device->CreateRasterizerState(&rd, &raster), "raster");
        ctx->RSSetState(raster.Get());
        const float clear[4] = {0, 0, 0, 0};
        ctx->ClearRenderTargetView(rtv.Get(), clear);
        ctx->Draw(3, 0);
        ctx->CopyResource(readback.Get(), rt.Get());
        D3D11_MAPPED_SUBRESOURCE mapped;
        checked(ctx->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "map");
        float actual[4];
        memcpy(actual, mapped.pData, 16);
        ctx->Unmap(readback.Get(), 0);
        const float want = ccw ? -1.0f : 1.0f;
        bool ok = actual[0] == want && actual[1] == want && actual[2] == want && actual[3] == want;
        ++checks;
        failures += !ok;
        printf("%s %s front face: FACING=(%g,%g,%g,%g) expected %g\n", ok ? "PASS" : "FAIL",
               ccw ? "CCW" : "CW", actual[0], actual[1], actual[2], actual[3], want);
    }
    printf("checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
