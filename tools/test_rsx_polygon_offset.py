"""Exercise actual polygon decoding, full guest PSO identity and API conversion."""

from pathlib import Path
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
VIDEO = ROOT / "libs/video"
OUT = ROOT / "build/polygon-offset-tests"
HARNESS = r"""

#include <stdio.h>
#include <string.h>
typedef unsigned int u32;
typedef unsigned long long u64;
typedef int INT;
typedef struct {
    INT DepthBias;
    float DepthBiasClamp, SlopeScaledDepthBias;
} D3D12_RASTERIZER_DESC;
#define NV4097_SET_POLY_OFFSET_FILL_ENABLE 0x0A68
#define NV4097_SET_POLYGON_OFFSET_SCALE_FACTOR 0x0A78
#define NV4097_SET_POLYGON_OFFSET_BIAS 0x0A7C
typedef struct {
    int cull_face_enable;
    u32 cull_face, front_face;
    int poly_offset_fill;
    float poly_offset_factor, poly_offset_units;
} rsx_state;
static int decode(rsx_state *state, u32 method, u32 data) { @DECODE@ return 1; }
@KEY@
typedef struct {
    u64 bias_key;
    u32 vs_idx, vs_hash, gen, blend, nrt, rtfmt, exp32, ucode_hash, cmask, cull, cube_mask;
    u64 alias_key;
} Entry;
static struct {
    Entry vp_fp[1];
    u32 vp_gen;
} s_d3d;
static int matches(u64 bias_key) {
    int i = 0, vs_idx = 0, nrt = 0, exp32 = 0;
    u32 vs_hash = 0, blend = 0, rtfmt = 0, uhash = 0, cmask = 0, cull = 0, cube_mask = 0;
    u64 alias_key = 0;
    return @MATCH@;
}
static u32 bits(float v) {
    u32 u;
    memcpy(&u, &v, 4);
    return u;
}
#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);                           \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)
int main(void) {
    struct {
        float factor, units;
        INT expected;
    } cases[] = {{2, 0, 0},
                 {1, 1, 1},
                 {-1.5f, -4, -4},
                 {0, 2047, 2047},
                 {0.015625f, 0, 0},
                 {2.00390625f, 65536, 65536},
                 {0.00000011920928955078125f, -65536, -65536},
                 {0, 2147483648.0f, 2147483647},
                 {0, -2147483648.0f, (-2147483647 - 1)},
                 {0, 2147483520.0f, 2147483520},
                 {1, 0.5f, 1},
                 {1, -0.5f, -1}};
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        rsx_state state = {0};
        state.front_face = 0x0901;
        state.cull_face = 0x0405;
        CHECK(!decode(&state, NV4097_SET_POLY_OFFSET_FILL_ENABLE, 1));
        CHECK(!decode(&state, NV4097_SET_POLYGON_OFFSET_SCALE_FACTOR, bits(cases[i].factor)));
        CHECK(!decode(&state, NV4097_SET_POLYGON_OFFSET_BIAS, bits(cases[i].units)));
        u64 key = rsx_polygon_offset_key(&state);
        CHECK((u32)(key >> 32) == bits(cases[i].factor) && (u32)key == bits(cases[i].units));
        D3D12_RASTERIZER_DESC raster = {123, 123, 123};
        rsx_polygon_depth_bias(key, &raster);
        CHECK(raster.DepthBias == cases[i].expected &&
              bits(raster.SlopeScaledDepthBias) == bits(cases[i].factor) &&
              raster.DepthBiasClamp == 0);
        CHECK(rsx_cull_key(&state) == 4);
        state.cull_face_enable = 1;
        CHECK(rsx_cull_key(&state) == 5);
        state.poly_offset_fill = 0;
        CHECK(rsx_polygon_offset_key(&state) == 0);
        raster = (D3D12_RASTERIZER_DESC){123, 123, 123};
        rsx_polygon_depth_bias(0, &raster);
        CHECK(!raster.DepthBias && !raster.SlopeScaledDepthBias && !raster.DepthBiasClamp);
    }
    rsx_state state = {0};
    state.poly_offset_fill = 1;
    state.poly_offset_factor = 2;
    u64 first = rsx_polygon_offset_key(&state);
    state.poly_offset_factor = 2.00390625f;
    u64 second = rsx_polygon_offset_key(&state);
    CHECK(first != second);
    s_d3d.vp_fp[0].bias_key = first;
    CHECK(matches(first) && !matches(second));
    state.poly_offset_factor = 2;
    state.poly_offset_units = 0.25f;
    second = rsx_polygon_offset_key(&state);
    CHECK(first != second && !matches(second));
    puts("PASS 12 decode/API cases, exact float identity, winding, disabled state and actual cache "
         "predicate");
    return 0;
}
"""


def parts(backend, commands):
    start = backend.index("static u32 rsx_cull_key(")
    key = backend[start : backend.index("\nstatic u32 rsx_blend_key(", start)]
    decode = re.search(
        r"    /\* Polygon offset: fill enable.*?OFFSET_BIAS\) \{.*?return 0;\n    \}\n",
        commands,
        re.S,
    )
    assert decode, "polygon decoder absent"
    match = re.search(
        r"else if \((s_d3d\.vp_fp\[i\]\.vs_idx == vs_idx.*?)\)\s*return s_d3d\.vp_fp\[i\]\.pso;",
        backend,
        re.S,
    )
    assert match, "cache predicate absent"
    return (
        HARNESS.replace("@KEY@", key)
        .replace("@DECODE@", decode[0])
        .replace("@MATCH@", match[1])
    )


def audit(backend):
    assert (
        backend.count("    rsx_polygon_depth_bias(bias_key, &pd.RasterizerState);") == 1
    ), "PSO hookup"
    assert (
        backend.count("dr->bias_key = rsx_polygon_offset_key(s_d3d.current_rsx_state);")
        == 3
    ), "record capture"
    assert (
        "recipe.bias=bias_key;" in backend and "cached->bias_key = bias_key;" in backend
    ), "stored identity"
    assert backend.count("dr->cull, dr->bias_key,") == 2, "replay/prefetch arguments"
    assert "pv->bias_key == rsx_polygon_offset_key(state)" in backend, "merged identity"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler")
    args = parser.parse_args()
    compiler = (
        args.compiler
        or os.environ.get("MV_CLANG")
        or os.environ.get("CC")
        or ("clang-cl" if os.name == "nt" else "cc")
    )
    compiler = shutil.which(compiler) or compiler
    msvc = Path(compiler).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
    OUT.mkdir(parents=True, exist_ok=True)
    backend = (VIDEO / "rsx_d3d12_backend.c").read_text(encoding="utf-8")
    commands = (VIDEO / "rsx_commands.c").read_text(encoding="utf-8")
    audit(backend)
    results = []

    def run(name, b, c):
        source = OUT / (name + ".c")
        source.write_text(parts(b, c), encoding="utf-8")
        exe = OUT / (name + (".exe" if os.name == "nt" else ""))
        command = (
            [
                compiler,
                "/nologo",
                "/std:c17",
                "/D_CRT_SECURE_NO_WARNINGS",
                str(source),
                "/Fe" + str(exe),
            ]
            if msvc
            else [
                compiler,
                "-std=c17",
                "-O2",
                "-fsanitize=undefined,float-cast-overflow",
                str(source),
                "-o",
                str(exe),
            ]
        )
        built = subprocess.run(command, cwd=OUT, capture_output=True, text=True)
        (OUT / (name + "-build.log")).write_text(
            built.stdout + built.stderr, encoding="utf-8"
        )
        assert not built.returncode, built.stderr
        tested = subprocess.run([str(exe)], capture_output=True, text=True)
        (OUT / (name + "-run.log")).write_text(
            tested.stdout + tested.stderr, encoding="utf-8"
        )
        results.append(
            {"name": name, "command": command, "returncode": tested.returncode}
        )
        return tested

    result = run("production", backend, commands)
    assert not result.returncode, result.stdout + result.stderr
    mutants = {
        "decode-dropped": (
            backend,
            commands.replace(
                "state->poly_offset_fill = data ? 1 : 0;",
                "state->poly_offset_fill = 0;",
            ),
        ),
        "factor-quantized": (
            backend.replace(
                "memcpy(&rd->SlopeScaledDepthBias, &factor_bits, sizeof factor_bits);",
                "memcpy(&rd->SlopeScaledDepthBias, &factor_bits, sizeof factor_bits); rd->SlopeScaledDepthBias = (float)(int)(rd->SlopeScaledDepthBias * 16) / 16;",
            ),
            commands,
        ),
        "units-clamped": (
            backend.replace(
                "rd->DepthBiasClamp = 0.0f;",
                "if (rd->DepthBias > 2047) rd->DepthBias = 2047; rd->DepthBiasClamp = 0.0f;",
            ),
            commands,
        ),
        "cache-omitted": (
            backend.replace("s_d3d.vp_fp[i].bias_key == bias_key &&", "1 &&"),
            commands,
        ),
        "winding-omitted": (
            backend.replace(
                "u32 key = st->front_face == 0x0901u ? 4u : 0u;", "u32 key = 0;"
            ),
            commands,
        ),
    }
    for name, (b, c) in mutants.items():
        assert b != backend or c != commands, name
        result = run(name, b, c)
        assert result.returncode, name + " survived"
    for old in (
        "rsx_polygon_depth_bias(bias_key, &pd.RasterizerState);",
        "dr->bias_key = rsx_polygon_offset_key(s_d3d.current_rsx_state);",
        "recipe.bias=bias_key;",
        "cached->bias_key = bias_key;",
        "dr->cull, dr->bias_key,",
        "pv->bias_key == rsx_polygon_offset_key(state)",
    ):
        try:
            audit(backend.replace(old, old.replace("bias_key", "missing_bias_key"), 1))
        except AssertionError:
            pass
        else:
            raise AssertionError("wiring control survived: " + old)
    report = {
        "results": results,
        "compiled_mutants": list(mutants),
        "wiring_controls": 6,
        "backend_sha256": hashlib.sha256(backend.encode()).hexdigest(),
    }
    (OUT / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(
        result.stdout.strip()
        if result.returncode == 0
        else "PASS polygon offset, five compiled mutants and six wiring controls"
    )


if __name__ == "__main__":
    main()
