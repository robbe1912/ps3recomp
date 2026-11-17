"""Compile the production coherent CPU surface path, fence helper and surface prefetch
against a fake D3D12 GPU timeline; check stopping cases, wiring and mutants."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
START = "typedef struct CpuSurfaceRequest {"
STOP = "static void cpu_surface_snapshot_trace("
PREFETCH = "/* Prefetched copies of opted-in surfaces."
FENCE = "/* GPU fence completion."
FAIL_STOP = "[CPUSURFACE] "


def exe_name():
    return "check.exe" if os.name == "nt" else "check"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler")
    args = parser.parse_args()
    source = (ROOT / "libs/video/rsx_cpu_surface.inc").read_text(encoding="utf-8")
    for marker in (START, STOP, PREFETCH, FENCE):
        assert source.count(marker) == 1, marker
    assert (
        source.index(START)
        < source.index("static void cpu_surface_fence_wait(void)")
        < source.index(FENCE)
        < source.index(PREFETCH)
        < source.index("static void cpu_surface_copy(OffRT *r)")
        < source.index(STOP)
    )
    region = source[source.index(START) : source.index(STOP)]
    assert "RSX_CPU_SURFACE_ASYNC" not in source, "no stale-read surface list remains"
    # Wiring outside the compiled region.
    init = source[
        source.index("static void cpu_surface_init") : source.index(
            "static void cpu_surface_retire"
        )
    ]
    assert init.index("cpu_surface_prefetch_init();") < init.index(
        "vm_read_sync_set_handler(cpu_surface_request,NULL);"
    ), "one coherent read handler"
    retire = source[
        source.index("static void cpu_surface_retire") : source.index(
            "static void cpu_surface_shutdown"
        )
    ]
    assert (
        retire.index("cpu_surface_prefetch_retire(r);")
        < retire.index("cpu_surface_copy(r);")
        < retire.index("->Release(r->cpu_readback)")
    )
    shutdown = source[
        source.index("static void cpu_surface_shutdown") : source.index(
            "static void cpu_surface_capture"
        )
    ]
    assert (
        shutdown.index("vm_read_sync_set_handler(NULL,NULL);")
        < shutdown.index("cpu_surface_prefetch_shutdown();")
        < shutdown.index("cpu_surface_retire(")
    )
    issue = region[
        region.index("static void cpu_surface_prefetch_issue") : region.index(
            "static void cpu_surface_prefetch_print"
        )
    ]
    assert issue.index("ExecuteCommandLists") < issue.index(
        "Signal(s_d3d.cmd_queue,s_cpu_surface_prefetch_fence,++s_cpu_surface_prefetch_fence_value)"
    ), "fence signalled behind the copies"
    backend = (ROOT / "libs/video/rsx_d3d12_backend.c").read_text(encoding="utf-8")
    body = backend[backend.index("static void render_frame(void)\n{") :]
    body = body[: body.index("\nstatic ")]
    assert body.count("cpu_surface_prefetch_refresh();") == 1
    assert (
        body.index("submit_recorded_commands();")
        < body.index("cpu_surface_batch_submitted();")
        < body.index("cpu_surface_prefetch_refresh();")
        < body.index("cpu_surface_frame_complete();")
    ), "prefetch copies only submitted work"
    out = ROOT / "build/cpu-surface-prefetch-tests"
    out.mkdir(parents=True, exist_ok=True)
    fixture = (ROOT / "libs/video/tests/test_cpu_surface_prefetch.c").read_text(
        encoding="utf-8"
    )
    report = {
        "qualification": "Production bodies against an authored GPU model. Actual D3D12 coverage runs separately on Windows.",
        "inputs_sha256": {
            name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
            for name in [
                "libs/video/rsx_cpu_surface.inc",
                "libs/video/rsx_d3d12_backend.c",
                "libs/video/tests/test_cpu_surface_prefetch.c",
                "tools/test_cpu_surface_prefetch.py",
            ]
        },
        "commands": [],
        "mutants_rejected": [],
    }

    def save():
        (out / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")

    assert fixture.count("/* PRODUCTION */") == 1
    compiler = (
        args.compiler
        or os.environ.get("MV_CLANG")
        or os.environ.get("CC")
        or ("clang-cl" if os.name == "nt" else "cc")
    )

    def build(name, text):
        folder = out / name
        folder.mkdir(exist_ok=True)
        c = folder / "check.c"
        c.write_text(fixture.replace("/* PRODUCTION */", text), encoding="utf-8")
        exe = folder / exe_name()
        inc = "libs/video"
        if Path(compiler).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            cmd = [
                compiler,
                "/nologo",
                "/O2",
                "/UNDEBUG",
                "/std:c11",
                "/I" + str(ROOT / inc),
                str(c),
                "/Fo" + str(folder) + os.sep,
                "/Fe" + str(exe),
            ]
        else:
            cmd = [
                compiler,
                "-std=c11",
                "-O2",
                "-UNDEBUG",
                "-Wall",
                "-Wextra",
                "-Wno-unused-function",
                "-Wno-unused-parameter",
                "-I" + str(ROOT / inc),
                str(c),
                "-o",
                str(exe),
            ]
        r = subprocess.run(cmd, capture_output=True, text=True)
        (folder / "build.log").write_text(r.stdout + r.stderr, encoding="utf-8")
        report["commands"].append(
            {"command": cmd, "returncode": r.returncode, "kind": "build"}
        )
        save()
        assert r.returncode == 0, r.stdout + r.stderr
        return exe

    def run(exe, *args):
        r = subprocess.run(
            [str(exe), *args], capture_output=True, text=True, timeout=60
        )
        number = len(report["commands"])
        (out / f"run-{number:04d}.log").write_text(
            r.stdout + r.stderr, encoding="utf-8"
        )
        report["commands"].append(
            {"command": [str(exe), *args], "returncode": r.returncode, "kind": "run"}
        )
        save()
        return r

    def stops(r, message):
        return (
            r.returncode not in (0, 1)
            and FAIL_STOP + message in r.stderr
            and "FAIL:" not in r.stderr
        )

    STOPS = [
        ("removed-prefetch", "GPU fence (device removed) failed"),
        ("removed-refresh", "GPU fence (device removed) failed"),
        (
            "removed-coherent",
            "s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device) failed",
        ),
        ("removed-while-waiting", "GPU fence (device removed) failed"),
        ("recording", "surface prefetch before command submission failed"),
    ]
    BAD_CONFIGS = [
        "zz",
        "1,2,3,4,5",
        "100000000",
        "1,",
        ",1",
        "1,,2",
        "1F00,1f00",
        "-1",
        " 1",
        "0x1F00",
        "1F00 ",
    ]

    def full_pass(exe):
        r = run(exe)
        if r.returncode != 0 or "PASS" not in r.stdout:
            return (
                "scenario: "
                + (r.stderr.strip().splitlines() or ["exit %d" % r.returncode])[-1]
            )
        for mode, message in STOPS:
            if not stops(run(exe, mode), message):
                return "stop " + mode
        for bad in BAD_CONFIGS:
            if not stops(run(exe, "config", bad), "RSX_CPU_SURFACE_PREFETCH"):
                return "config %r accepted" % bad
        return None

    exe = build("actual", region)
    failure = full_pass(exe)
    assert failure is None, failure
    print(run(exe).stdout.strip())
    print(
        "PASS stops: "
        + ", ".join(m for m, _ in STOPS)
        + "; "
        + str(len(BAD_CONFIGS))
        + " malformed lists rejected"
    )
    mutants = [
        (
            "take-ignores-generation",
            "!e->generation || e->generation!=r->gpu_generation || ",
            "!e->generation || ",
        ),
        (
            "take-ignores-layout",
            "r->cpu_address!=e->address || r->cpu_pitch!=e->pitch || ",
            "r->cpu_address!=e->address || ",
        ),
        (
            "take-skips-fence",
            '        e->waited++;cpu_surface_fence_block(s_cpu_surface_prefetch_fence,s_cpu_surface_prefetch_event,e->fence,"prefetch GPU fence wait");\n',
            "        e->waited++;\n",
        ),
        (
            "removal-counts-as-complete",
            "    if(completed==UINT64_MAX) {\n",
            "    if(0) {\n",
        ),
        (
            "wait-skips-recheck",
            "WaitForSingleObject(event,30000)!=WAIT_OBJECT_0 || !cpu_surface_fence_reached(fence,value)",
            "WaitForSingleObject(event,30000)!=WAIT_OBJECT_0",
        ),
        (
            "reuses-busy-allocator",
            "    if(!cpu_surface_fence_reached(s_cpu_surface_prefetch_fence,s_cpu_surface_prefetch_fence_value))return;\n",
            "",
        ),
        (
            "copies-without-reads",
            "if(r && e->wanted && r->gpu_generation",
            "if(r && r->gpu_generation",
        ),
        ("reads-never-noted", "            cpu_surface_prefetch_note(r);\n", ""),
        ("copy-skips-prefetch", "    if(cpu_surface_prefetch_take(r))return;", ""),
        (
            "retire-keeps-copy",
            "    cpu_surface_prefetch_untrack(e);\n}\nstatic void cpu_surface_prefetch_init",
            "}\nstatic void cpu_surface_prefetch_init",
        ),
        (
            "retire-skips-wait",
            '    if(e->generation)cpu_surface_fence_block(s_cpu_surface_prefetch_fence,s_cpu_surface_prefetch_event,e->fence,"prefetch GPU fence wait");\n',
            "",
        ),
        ("offset-overflows", "if(++digits>8)", "if(++digits>16)"),
        (
            "duplicate-offsets",
            "            if(s_cpu_surface_prefetch[i].off==(u32)off)cpu_surface_fail",
            "            if(0)cpu_surface_fail",
        ),
    ]
    for name, before, after in mutants:
        assert region.count(before) == 1, (name, before)
        bad = build(name, region.replace(before, after))
        failure = full_pass(bad)
        assert failure is not None, "mutant survived: " + name
        report["mutants_rejected"].append(name)
        save()
        print("PASS mutant " + name + " rejected: " + failure)
    report["passed"] = True
    save()
    print(
        "PASS: coherent CPU surface prefetch and wiring ("
        + str(len(mutants))
        + " mutants rejected)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
