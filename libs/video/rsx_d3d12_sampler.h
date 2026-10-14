/* Guest sampler registers shared by the D3D12 binder and GPU regression. */
#ifndef PS3RECOMP_RSX_D3D12_SAMPLER_H
#define PS3RECOMP_RSX_D3D12_SAMPLER_H
#include <d3d12.h>
#include <stdio.h>
#include <string.h>
/* The generator declares a 16-element sampler bank but emits constant-index
 * references only for units actually sampled. Unused inherited registers
 * must not enter sampler validation or immutable-table identity. */
static unsigned rsx_hlsl_sampler_mask(const char *hlsl) {
  unsigned mask = 0;
  char token[32];
  for (unsigned u = 0; u < 16; ++u) {
    snprintf(token, sizeof token, "rsx_samp[%u]", u);
    if (strstr(hlsl, token))
      mask |= 1u << u;
  }
  return mask;
}
static int rsx_d3d12_address(unsigned value, D3D12_TEXTURE_ADDRESS_MODE *out) {
  switch (value) {
  case 3:
    *out = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    return 1;
  case 1:
    *out = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    return 1;
  case 2:
    *out = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    return 1;
  case 4:
    *out = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    return 1;
  case 6:
    *out = D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    return 1;
  default:
    return 0; /* legacy clamp/border mirror modes need shader support */
  }
}
static int rsx_d3d12_sampler(unsigned address, unsigned filter,
                             unsigned control0, unsigned border,
                             D3D12_SAMPLER_DESC *out) {
  memset(out, 0, sizeof *out);
  if (!rsx_d3d12_address(address & 15, &out->AddressU) ||
      !rsx_d3d12_address((address >> 8) & 15, &out->AddressV) ||
      !rsx_d3d12_address((address >> 16) & 15, &out->AddressW))
    return 0;
  unsigned minf = (filter >> 16) & 7, magf = (filter >> 24) & 7;
  if (minf < 1 || minf > 6 || magf < 1 || magf > 2)
    return 0;
  unsigned minlin = minf == 2 || minf == 4 || minf == 6, maglin = magf == 2,
           miplin = minf == 5 || minf == 6;
  out->Filter =
      (D3D12_FILTER)((minlin ? 0x10 : 0) | (maglin ? 4 : 0) | (miplin ? 1 : 0));
  static const unsigned aniso[8] = {1, 2, 4, 6, 8, 10, 12, 16};
  out->MaxAnisotropy = aniso[(control0 >> 4) & 7];
  if (out->MaxAnisotropy > 1 && minlin && maglin)
    out->Filter = D3D12_FILTER_ANISOTROPIC;
  int bias = (int)(filter & 8191);
  if (bias & 4096)
    bias -= 8192;
  out->MipLODBias = (float)bias / 256.0f;
  out->MinLOD = (float)((control0 >> 19) & 4095) / 256.0f;
  out->MaxLOD = minf >= 3 ? (float)((control0 >> 7) & 4095) / 256.0f : 0;
  if (out->MaxLOD < out->MinLOD)
    out->MaxLOD = out->MinLOD;
  out->ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
  out->BorderColor[0] = (float)((border >> 16) & 255) / 255.0f;
  out->BorderColor[1] = (float)((border >> 8) & 255) / 255.0f;
  out->BorderColor[2] = (float)(border & 255) / 255.0f;
  out->BorderColor[3] = (float)(border >> 24) / 255.0f;
  return 1;
}
#endif
