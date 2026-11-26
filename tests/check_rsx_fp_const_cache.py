"""Compile the FP snapshot constant cache with the production snapshot functions and the
production draw record, against the draw record of before the cache. Rejects mutants.

Usage (from any directory):
  python tests/check_rsx_fp_const_cache.py [--out DIR] [--src LIBS_VIDEO_DIR]
The compiler is MV_CLANG, CC, or clang-cl on Windows (cc on other systems).
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SNAP_START = "#define VP_FPSNAP_MAX 512"
SNAP_END = "static ID3D12PipelineState* vp_get_fp_pso("
RECORD_START = "static void vp_record_fp_b1(u32 slot, D3D12DrawRecord* dr)\n{"
RECORD_END = "\n}\n"

# vp_record_fp_b1 of commit 4630c6bd (before the cache), with another name and without
# its FPK_DBG print block and its comments. The statements are those of the base commit.
OLD_PATH = """static void old_record_fp_b1(u32 slot, D3D12DrawRecord* dr)
{
    if (!s_d3d.vp_fpcb_mapped || slot >= MAX_DRAWS || !dr || !dr->fp_addr) return;
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int, u32);
    u32* ks = (u32*)((char*)s_d3d.vp_fpcb_mapped
        + ((u64)s_d3d.vp_parity * MAX_DRAWS + slot) * VP_FPCB_STRIDE);
    u32 foff = cellGcmResolveLocated((dr->fp_addr & 0x3u) == 1, dr->fp_addr & ~0x3u);
    static rsx_fp_constant_block blk;  /* 32 KB scratch; record path is single-threaded */
    u32 n = 0;
    if (vm_base && foff != 0xFFFFFFFFu) {
        const u8* uc = vp_fp_snap_bytes(dr->fp_snap, foff);
        if (!uc) uc = vm_base + foff;
        int got = rsx_fp_collect_constants(uc, VP_FP_UCODE_MAX, &blk);
        if (got > 0) n = (u32)got;      /* <=0: malformed; the PSO build refuses it too */
    }
    if (n > VP_FPCB_MAX_CONSTS) {
        static u32 seen[8]; static int ns = 0; int known = 0;
        for (int i = 0; i < ns; i++) if (seen[i] == dr->fp_addr) known = 1;
        if (!known && ns < 8) { seen[ns++] = dr->fp_addr;
            printf("[FP] b1 snapshot refused (fp=0x%08X): %u inline constants"
                   " exceed the slot (%u max)\\n",
                   dr->fp_addr, n, (u32)VP_FPCB_MAX_CONSTS); }
        dr->fp_const_n = 0xFFFFu;
        n = 0;
    } else {
        dr->fp_const_n = n;
        if (n) memcpy(ks, blk.values, (size_t)n * 16u);
    }
    float ref = rsx_env_once("NO_ALPHATEST")
        ? 0.0f : rsx_fp_alpha_ref(dr->alpha_ref_raw, dr->rt_fmt);
    u32 shader_n = n ? n : 1u;
    memcpy(ks + shader_n * 4u, &ref, 4);
}
"""

COLLECTOR = """#define rsx_fp_collect_constants rsx_fp_collect_constants_real
#include "rsx_fp_decompiler.c"
#undef rsx_fp_collect_constants
int g_collects;
int rsx_fp_collect_constants(const u8* ucode, u32 max_bytes, rsx_fp_constant_block* out)
{
    g_collects++;
    return rsx_fp_collect_constants_real(ucode, max_bytes, out);
}
"""

RESET = "    rsx_fp_const_cache_reset(&s_fp_const_cache[idx]);   /* the slot holds a new program */\n"
COUNT_STORE = "    entry->count = out->count;\n"

# name, part, text before, text after. Each mutant keeps the code compilable.
MUTANTS = [
    ("cache keyed by program address, not by snapshot slot", "record",
     "&s_fp_const_cache[dr->fp_snap - 1] : NULL,", "&s_fp_const_cache[(foff >> 2) % VP_FPSNAP_MAX] : NULL,"),
    ("no reset when a snapshot slot is reused", "snap", RESET, ""),
    ("overflow result cached wrongly", "header", COUNT_STORE,
     "    entry->count = out->count <= max_constants ? out->count : 0u;\n"),
    ("alpha reference not written per draw", "record",
     "    memcpy(ks + shader_n * 4u, &ref, 4);", "    if (n) memcpy(ks + shader_n * 4u, &ref, 4);"),
    ("draw without a verifying snapshot served from the cache", "record",
     "const VPFP_snap* snap = vp_fp_snap_entry(dr->fp_snap, foff);",
     "const VPFP_snap* snap = (dr->fp_snap && dr->fp_snap <= VP_FPSNAP_MAX)\n"
     "            ? &s_fp_snap[dr->fp_snap - 1] : NULL;"),
    ("stale count with valid values", "header", COUNT_STORE, ""),
    ("cache never used", "header", "    entry->valid = 1;\n", ""),
    ("only the first constant stored", "header",
     "memcpy(entry->values, scratch->values, (size_t)out->count * 16u);",
     "memcpy(entry->values, scratch->values, 16u);"),
]


def section(text, start, end):
    """The text from `start` up to `end` (exclusive), or through `end` when `end` is RECORD_END."""
    assert text.count(start) == 1, start
    i = text.index(start)
    j = text.index(end, i)
    return text[i:j + len(end)] if end == RECORD_END else text[i:j]


def replaced(text, before, after):
    assert text.count(before) == 1, before
    return text.replace(before, after)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=ROOT / "out" / "fp-const-cache")
    parser.add_argument("--src", type=Path, default=ROOT / "libs/video")
    arguments = parser.parse_args()
    out = arguments.out
    out.mkdir(parents=True, exist_ok=True)
    backend = (arguments.src / "rsx_d3d12_backend.c").read_text(encoding="utf-8")
    parts = {
        "snap": section(backend, SNAP_START, SNAP_END),
        "record": section(backend, RECORD_START, RECORD_END),
        "header": (arguments.src / "rsx_fp_const_cache.h").read_text(encoding="utf-8"),
    }
    fixture = (ROOT / "tests/fixtures/rsx_fp_const_cache.c").read_text(encoding="utf-8")
    compiler = os.environ.get("MV_CLANG") or os.environ.get("CC") or ("clang-cl" if os.name == "nt" else "cc")
    msvc = Path(compiler).name.lower() in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"}
    include = [str(arguments.src), str(arguments.src.parent.parent / "include")]

    def build(name, current):
        folder = out / name
        folder.mkdir(exist_ok=True)
        c = folder / "check.c"
        text = fixture
        for mark, value in (("/* PRODUCTION_SNAP */", current["snap"]), ("/* OLD_PATH */", OLD_PATH),
                            ("/* PRODUCTION_RECORD */", current["record"])):
            assert text.count(mark) == 1, mark
            text = text.replace(mark, value)
        c.write_text(text, encoding="utf-8")
        (folder / "collector.c").write_text(COLLECTOR, encoding="utf-8")
        (folder / "rsx_fp_const_cache.h").write_text(current["header"], encoding="utf-8")
        exe = folder / ("check.exe" if os.name == "nt" else "check")
        if msvc:
            flags = ["/nologo", "/O2", "/UNDEBUG", "/std:c17", *("/I" + p for p in include),
                     str(c), str(folder / "collector.c"), "/Fo" + str(folder) + os.sep, "/Fe" + str(exe)]
        else:
            flags = ["-std=c11", "-O2", "-UNDEBUG", *("-I" + p for p in include),
                     str(c), str(folder / "collector.c"), "-o", str(exe)]
        cmd = [compiler, *flags]
        (folder / "command.json").write_text(json.dumps(cmd, indent=2))
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        (folder / "build.log").write_text(r.stdout + r.stderr)
        assert r.returncode == 0, name + " does not build:\n" + r.stdout + r.stderr
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        (folder / "run.log").write_text(r.stdout + r.stderr)
        return r

    r = build("actual", parts)
    print(r.stdout + r.stderr, end="")
    if r.returncode:
        return r.returncode
    rejected = []
    for name, part, before, after in MUTANTS:
        current = dict(parts, **{part: replaced(parts[part], before, after)})
        r = build("mutant_" + str(len(rejected) + 1), current)
        lines = [line for line in (r.stdout + r.stderr).splitlines() if line.startswith("FAIL")]
        if r.returncode == 0:
            print("FAIL mutant survived: " + name)
            return 1
        rejected.append(name)
        print("PASS mutant rejected: " + name + (" -- " + lines[0][5:] if lines else ""))
    print(f"PASS: FP snapshot constant cache ({len(rejected)} mutants rejected)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
