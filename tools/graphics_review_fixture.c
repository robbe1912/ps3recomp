#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint8_t u8;
#define RSX_DSK_DEPTH_EN 1
#define RSX_DSK_WRITE 2
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "CHECK line %d: %s\n", __LINE__, #x);                    \
      return 1;                                                                \
    }                                                                          \
  } while (0)
typedef struct {
  unsigned initialized, width, height;
} rsx_d3d12_depth_surface;
typedef struct {
  u32 depth_slot, is_clear, clear_flags, ds_key, cmask, rt_off, rt_mrt[3];
  struct {
    u32 raw, w, h, fmt, ctrl1, mips, depth;
    int cube, set;
  } tex[16];
  u8 depth_alias_index[16];
  u32 depth_alias_order[16], depth_alias_mask;
} D3D12DrawRecord;
static struct {
  struct {
    rsx_d3d12_depth_surface surfaces[4];
  } depth_cache;
} s_d3d;
static u8 s_alias_colour_last[5];
static int entries;
static unsigned native_depth_slot_at(u32 address) {
  return address && address <= 0x4000 && !(address & 0xfff) ? address / 0x1000
                                                            : 0;
}
static void native_depth_notice(const D3D12DrawRecord *d, unsigned u,
                                const char *s) {
  (void)d;
  (void)u;
  (void)s;
}
static int depth_alias_entry(unsigned slot, u32 w, u32 h) {
  (void)slot;
  (void)w;
  (void)h;
  ++entries;
  return 0;
}
/* PRODUCTION */
int main(int argc, char **argv) {
  CHECK(argc == 4);
  D3D12DrawRecord d = {0};
#ifdef CHECK_VOLUME
  if (!strcmp(argv[1], "volume")) {
    d.tex[0].set = 1;
    d.tex[0].depth = 16;
    d.tex[0].fmt = 0x81;
    d.tex[1].set = 1;
    d.tex[1].cube = 1;
    d.tex[1].depth = 6;
    d.tex[1].fmt = 0x85;
    u32 expected = (atoi(argv[2]) ? 2u : 0u) | (atoi(argv[3]) ? 0x10000u : 0u);
    CHECK(dr_cube_mask(&d) == expected);
    CHECK(tex_unit_volume(&d, 0) == !!atoi(argv[3]));
    return 0;
  }
#endif
  u32 last[5] = {0};
  u8 written[5] = {0};
  for (unsigned i = 0; i < 4; ++i) {
    s_d3d.depth_cache.surfaces[i].initialized = 1;
    s_d3d.depth_cache.surfaces[i].width = 16;
    s_d3d.depth_cache.surfaces[i].height = 16;
  }
  d.depth_slot = 1;
  d.ds_key = 3;
  depth_alias_note_writes(&d, 0, last);
  CHECK(last[1] == 1);
  memset(&d, 0, sizeof d);
  d.is_clear = 1;
  d.rt_off = 0x1000;
  d.rt_mrt[0] = 0x2000;
  d.rt_mrt[1] = 0x3000;
  d.rt_mrt[2] = 0x4000;
  depth_alias_note_writes(&d, 1, last);
  for (unsigned slot = 1; slot <= 4; ++slot) {
    CHECK(s_alias_colour_last[slot]);
    d.tex[0].raw = slot * 0x1000;
    d.tex[0].w = d.tex[0].h = 16;
    d.depth_alias_mask = 0;
    depth_alias_prepare(&d, 0, written, last);
    CHECK(d.depth_alias_mask == 0);
  }
  CHECK(entries == 0);
  memset(s_alias_colour_last, 0, sizeof s_alias_colour_last);
  d.is_clear = 0;
  d.cmask = 0xf0;
  depth_alias_note_writes(&d, 2, last);
  CHECK(!s_alias_colour_last[1] && s_alias_colour_last[2] &&
        !s_alias_colour_last[3] && !s_alias_colour_last[4]);
  d.depth_slot = 2;
  d.cmask = 0;
  d.ds_key = 3;
  depth_alias_note_writes(&d, 3, last);
  CHECK(!s_alias_colour_last[2] && last[2] == 4);
  d.is_clear = 1;
  d.depth_slot = 1;
  d.clear_flags = 3;
  depth_alias_note_writes(&d, 4, last);
  CHECK(!s_alias_colour_last[1] && s_alias_colour_last[2] && last[1] == 5);
  d.tex[0].raw = 0x1000;
  d.depth_alias_mask = 0;
  depth_alias_prepare(&d, 0, written, last);
  CHECK(d.depth_alias_mask == 1 && entries == 1);
  d.clear_flags = 0;
  depth_alias_note_writes(&d, 5, last);
  CHECK(s_alias_colour_last[1] && last[1] == 5);
  return 0;
}
