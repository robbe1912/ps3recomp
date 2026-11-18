"""Compile the production eager CPU-surface submission against an authored renderer model, plus mutants."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MARK = "/* Eager submission after writes to listed surfaces."
END = "/* Prompt service between batches."


def section(text, start, end):
    assert text.count(start) == 1, start
    i = text.index(start)
    return text[i : text.index(end, i)]


def main():
    source = (ROOT / "libs/video/rsx_cpu_surface.inc").read_text(encoding="utf-8")
    backend = (ROOT / "libs/video/rsx_d3d12_backend.c").read_text(encoding="utf-8")
    fixture = (ROOT / "libs/video/tests/test_cpu_surface_eager.c").read_text(encoding="utf-8")
    eager = section(source, MARK, END)
    hexer = section(source, "static int cpu_surface_hex_digit", "static void cpu_surface_prefetch_configure")

    # Wiring: every recorded colour target is noted, the list comes from the
    # environment, the mark clears only after submission, and the decision
    # rides the existing early-submission hook.
    capture = section(source, "static void cpu_surface_capture", "static void cpu_surface_bind")
    note = "cpu_surface_eager_note(recorded_offset,draw->cpu_address[i]);"
    assert capture.count(note) == 1, "capture notes each resolved colour target"
    assert "u32 recorded_offset = i ? draw->rt_mrt[i - 1] : draw->rt_off;" in capture
    assert "recorded_offset && recorded_offset == st->surface_color_offset[source] && draw->cpu_pitch[i]" in capture
    assert capture.index("draw->cpu_address[i]=cellGcmResolveLocated") < capture.index(note), "note after the address resolves"
    chooser = "unsigned source=(!i&&st->color_target==2)?1:i;"
    dma = "if(dma!=CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER && dma!=CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER)continue;"
    assert chooser in capture and chooser in eager and dma in capture and dma in eager, "next-draw targets chosen as capture chooses them"
    init = section(source, "static void cpu_surface_init", "static void cpu_surface_retire")
    assert 'cpu_surface_eager_configure(getenv("RSX_CPU_SURFACE_EAGER"));' in init
    body = backend[backend.index("static void render_frame(void)") :]
    body = body[: body.index("\nstatic ")]
    assert (
        body.index("submit_recorded_commands();")
        < body.index("cpu_surface_batch_submitted();")
        < body.index("cpu_surface_eager_submitted();")
        < body.index("cpu_surface_prefetch_refresh();")
        < body.index("cpu_surface_frame_complete();")
    ), "mark clears after submission, before the prefetch refresh copies the new generation"
    flush = section(backend, "static void cpu_surface_flush_recorded", "static void ensure_draw_record_capacity")
    assert (
        flush.index("cpu_surface_flush_wanted(at_pump)")
        < flush.index("cpu_surface_eager_wanted(at_pump)")
        < flush.index("s_d3d.initialized")
        and "submit_record_capacity(NULL, 0);" in flush
    ), "eager submission takes the capacity-drain path, decided before GPU state is read"
    assert "s_d3d" not in flush[: flush.index("cpu_surface_eager_wanted(at_pump)") + len("cpu_surface_eager_wanted(at_pump)")], \
        "no renderer state is read before the owner checks"

    out = ROOT / "build/cpu-surface-eager-tests"
    out.mkdir(parents=True, exist_ok=True)
    compiler = os.environ.get("MV_CLANG") or os.environ.get("CC") or ("clang-cl" if os.name == "nt" else "cc")
    msvc = Path(compiler).name.lower() in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"}
    results = {}

    def build(name, block):
        folder = out / name
        folder.mkdir(exist_ok=True)
        c = folder / "check.c"
        c.write_text(
            fixture.replace("/* PRODUCTION_HEX */", hexer).replace("/* PRODUCTION_EAGER */", block),
            encoding="utf-8",
        )
        exe = folder / ("check.exe" if os.name == "nt" else "check")
        inc = [ROOT / "libs/video", ROOT / "include"]
        if msvc:
            cmd = [compiler, "/nologo", "/O2", "/UNDEBUG", *("/I" + str(p) for p in inc), "/std:c17",
                   str(c), "/Fo" + str(folder) + os.sep, "/Fe" + str(exe)]
        else:
            cmd = [compiler, "-std=c11", "-O2", "-UNDEBUG", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                   *("-I" + str(p) for p in inc), str(c), "-o", str(exe)]
        (folder / "command.json").write_text(json.dumps(cmd, indent=2))
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        (folder / "build.log").write_text(r.stdout + r.stderr)
        assert r.returncode == 0, r.stdout + r.stderr
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        (folder / "run.log").write_text(r.stdout + r.stderr)
        results[name] = {"build": 0, "run": r.returncode}
        return r

    r = build("actual", eager)
    print(r.stdout + r.stderr.replace("[CPUSURFACE_EAGER]", "  [CPUSURFACE_EAGER]"), end="")
    if r.returncode:
        return r.returncode
    mutants = [
        ("any-thread", "    if(InterlockedCompareExchange(&s_cpu_surface_thread,0,0)!=(LONG)GetCurrentThreadId())return 0;\n", ""),
        ("during-replay", "if(!s_cpu_surface_eager_dirty || s_cpu_surface_recording)return 0;", "if(!s_cpu_surface_eager_dirty)return 0;"),
        ("without-write", "if(!s_cpu_surface_eager_dirty || s_cpu_surface_recording)return 0;", "if(s_cpu_surface_recording)return 0;"),
        ("mid-pass", "    if(!at_pump && cpu_surface_eager_state_writes(s_d3d.current_rsx_state))return 0;\n", ""),
        ("pump-waits", "if(!at_pump && cpu_surface_eager_state_writes(", "if(cpu_surface_eager_state_writes("),
        ("state-before-owner", "    if(!s_cpu_surface_eager_count)return 0;\n",
         "    if(!s_cpu_surface_eager_count)return 0;\n    if(!s_d3d.current_rsx_state)return 0;\n"),
        ("no-clear", "    s_cpu_surface_eager_dirty=0;\n}", "    (void)0;\n}"),
        # Each mutant keeps every parameter referenced, so -Werror builds fail on behaviour, not warnings.
        ("unbound-marks", "if(address && address!=0xffffffffu && cpu_surface_eager_listed(offset))", "if(((void)address,cpu_surface_eager_listed(offset)))"),
        ("any-surface-marks", "&& cpu_surface_eager_listed(offset))", "&& ((void)offset,1))"),
        ("includes-inactive-targets", "for(unsigned i=0;i<targets;i++)",
         "for(unsigned i=0;i<(targets ? 4u : 0u);i++)"),
        ("target-b-slot0", "unsigned source=(!i&&st->color_target==2)?1:i;", "unsigned source=i;"),
        ("no-dma-check", "        if(dma!=CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER && dma!=CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER)continue;\n", "        (void)dma;\n"),
        ("duplicates", "            if(s_cpu_surface_eager[i]==(u32)off)cpu_surface_fail", "            if(0)cpu_surface_fail"),
        ("five-offsets", " || s_cpu_surface_eager_count==CPU_SURFACE_EAGER_MAX)", ")"),
        ("nine-digits", "if(++digits>8)", "if(++digits>9)"),
        ("empty-item", "if(!digits || (*list && *list!=',')", "if((*list && *list!=',')"),
    ]
    for name, before, after in mutants:
        assert eager.count(before) == 1, (name, before)
        bad = build(name, eager.replace(before, after))
        assert bad.returncode != 0 and "FAIL" in bad.stderr, (name, bad.returncode, bad.stdout, bad.stderr)
        print("PASS mutant " + name + " rejected: " + bad.stderr.strip().splitlines()[-1])
    (out / "result.json").write_text(
        json.dumps(
            {
                "commands": results,
                "mutants": [m[0] for m in mutants],
                "source_sha256": hashlib.sha256(source.encode("utf-8")).hexdigest(),
                "backend_sha256": hashlib.sha256(backend.encode("utf-8")).hexdigest(),
            },
            indent=2,
        )
    )
    print("PASS: eager CPU surface submission and wiring (" + str(len(mutants)) + " mutants rejected)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
