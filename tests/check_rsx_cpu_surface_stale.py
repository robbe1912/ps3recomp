"""Compile the production CPU surface read path, views, surface prefetch and the stale
read switch (RSX_CPU_SURFACE_STALE) against a fake D3D12 GPU timeline. Checks the
cases of the switch, the wiring, the behavior with the switch unset, and mutants.

Usage: python tests/check_rsx_cpu_surface_stale.py [--compiler <clang-cl>] [--out <dir>]
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
INC = ROOT / "libs/video"
START = "typedef struct CpuSurfaceRequest {"
STOP = "static void cpu_surface_snapshot_trace("
STALE = "/* Stale reads of one surface. RSX_CPU_SURFACE_STALE names one RSX surface"
PREFETCH = "/* Prefetched copies of opted-in surfaces."
FENCE = "/* GPU fence completion."
FAIL_STOP = "[CPUSURFACE] "
EXE = "check.exe" if os.name == "nt" else "check"


def compiler_environment(out):
    """MSVC-style environment and clang-cl of the latest Visual Studio, as the
    game's project.build_environment() finds them."""
    if os.environ.get("MV_CLANG") or os.environ.get("CC"):
        return os.environ.get("MV_CLANG") or os.environ.get("CC")
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)"))
    vswhere = vswhere / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.is_file():
        raise SystemExit("Visual Studio C++ build tools are required; use --compiler")
    line = subprocess.check_output(
        [str(vswhere), "-latest", "-products", "*", "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"],
        text=True).splitlines()
    if not line:
        raise SystemExit("Visual Studio C++ build tools are required; use --compiler")
    vcvars = Path(line[0].strip()) / "VC/Auxiliary/Build/vcvars64.bat"
    clang = Path(line[0].strip()) / "VC/Tools/Llvm/x64/bin/clang-cl.exe"
    if not clang.is_file():
        raise SystemExit("Install the Visual Studio LLVM/clang-cl tools, or use --compiler")
    task = out / "vcvars.cmd"
    task.write_text('@echo off\ncall "%s" >nul\nif errorlevel 1 exit /b 1\nset\n' % vcvars)
    output = subprocess.check_output(["cmd", "/d", "/c", str(task)], text=True)
    for entry in output.splitlines():
        if "=" in entry and not entry.startswith("="):
            key, value = entry.split("=", 1)
            os.environ[key] = value
    return str(clang)


def function(source, signature):
    """The body of one production function, by its first line."""
    assert source.count(signature) == 1, signature
    start = source.index(signature)
    end = source.index("\n}", start) + 2
    return source[start:end]


def wiring(source):
    """The switch hangs where the production code runs (outside the compiled region)."""
    assert "RSX_CPU_SURFACE_ASYNC" not in source, "no stale-read shadow list remains"
    init = function(source, "static void cpu_surface_init(void) {")
    assert init.index('cpu_surface_eager_configure(getenv("RSX_CPU_SURFACE_EAGER"));') < init.index(
        'cpu_surface_stale_configure(getenv("RSX_CPU_SURFACE_STALE"));') < init.index(
        "cpu_surface_prefetch_init();") < init.index("vm_read_sync_set_handler(cpu_surface_request,NULL);"), \
        "the stale switch is configured before the prefetch list reads it"
    prefetch_init = function(source, "static void cpu_surface_prefetch_init(void) {")
    assert prefetch_init.index("if(s_cpu_surface_stale.configured)") < prefetch_init.index(
        "if(!s_cpu_surface_prefetch_count)return;"), "the stale offset must be in the prefetch list"
    shutdown = function(source, "static void cpu_surface_shutdown(void) {")
    assert shutdown.index("vm_read_sync_set_handler(NULL,NULL);") < shutdown.index(
        "cpu_surface_stale_shutdown();") < shutdown.index("cpu_surface_prefetch_shutdown();"), \
        "the stale counters print after the last read"
    refresh = function(source, "static void cpu_surface_prefetch_refresh(void) {")
    assert refresh.index("OffRT *r=cpu_surface_prefetch_track(e);") < refresh.index(
        "cpu_surface_stale_land(e,r);") < refresh.index("cpu_surface_prefetch_issue("), \
        "a completed copy lands at each submitted batch, after the batch is tracked"
    written = function(source, "static void cpu_surface_written(int primary,const int secondary[3]) {")
    assert written.count("gpu_generation++;cpu_surface_view_update(") == 2, \
        "a recorded write moves the view: that is the permit of one stale read"
    bind = function(source, "static void cpu_surface_bind(int index,const D3D12DrawRecord *draw,unsigned target) {")
    assert bind.rstrip().endswith("cpu_surface_view_update(r);\n}"), "a bound surface publishes its view"
    retire = function(source, "static void cpu_surface_retire(OffRT *r) {")
    assert retire.index("cpu_surface_prefetch_retire(r);") < retire.index("cpu_surface_copy(r);") < retire.index(
        "->Release(r->cpu_readback)") < retire.index("cpu_surface_view_retire(r);"), \
        "a retired surface forgets its landed copy last"
    backend = (INC / "rsx_d3d12_backend.c").read_text(encoding="utf-8")
    body = backend[backend.index("static void render_frame(void)"):]
    body = body[:body.index("\nstatic ")]
    assert body.count("cpu_surface_prefetch_refresh();") == 1
    assert body.index("submit_recorded_commands();") < body.index("cpu_surface_batch_submitted();") < body.index(
        "cpu_surface_prefetch_refresh();") < body.index("cpu_surface_frame_complete();"), \
        "prefetch copies and landings follow each submitted batch"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler")
    parser.add_argument("--out", help="build folder (default: <repo>/build/self-tests/cpu-surface-stale)")
    args = parser.parse_args()
    source_path = INC / "rsx_cpu_surface.inc"
    source = source_path.read_text(encoding="utf-8")
    for marker in (START, STOP, STALE, PREFETCH, FENCE):
        assert source.count(marker) == 1, marker
    assert source.index(START) < source.index(STALE) < source.index("static void cpu_surface_fence_wait(void)") \
        < source.index(FENCE) < source.index(PREFETCH) < source.index("static void cpu_surface_copy(OffRT *r)") \
        < source.index(STOP), "the order of the blocks"
    wiring(source)
    region = source[source.index(START):source.index(STOP)]
    out = Path(args.out) if args.out else ROOT / "build/self-tests/cpu-surface-stale"
    out.mkdir(parents=True, exist_ok=True)
    fixture_path = ROOT / "tests/fixtures/cpu_surface_stale.c"
    fixture = fixture_path.read_text(encoding="utf-8")
    assert fixture.count("/* PRODUCTION */") == 1
    compiler = args.compiler or compiler_environment(out)
    msvc = Path(compiler).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
    report = {
        "qualification": "Production bodies against an authored GPU model. Actual D3D12 coverage runs separately on Windows.",
        "inputs_sha256": {name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest() for name in (
            "libs/video/rsx_cpu_surface.inc", "libs/video/rsx_d3d12_backend.c",
            "tests/fixtures/cpu_surface_stale.c", "tests/check_rsx_cpu_surface_stale.py")},
        "commands": [],
        "mutants_rejected": [],
    }

    def save():
        (out / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")

    def build(name, text):
        folder = out / name
        folder.mkdir(exist_ok=True)
        c = folder / "check.c"
        c.write_text(fixture.replace("/* PRODUCTION */", text), encoding="utf-8")
        exe = folder / EXE
        if msvc:
            cmd = [compiler, "/nologo", "/O2", "/UNDEBUG", "/std:c11", "/I" + str(INC), str(c),
                   "/Fo" + str(folder) + os.sep, "/Fe" + str(exe)]
        else:
            cmd = [compiler, "-std=c11", "-O2", "-UNDEBUG", "-Wall", "-Wextra", "-Wno-unused-function",
                   "-Wno-unused-parameter", "-I" + str(INC), str(c), "-o", str(exe)]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        (folder / "build.log").write_text(r.stdout + r.stderr, encoding="utf-8")
        report["commands"].append({"command": cmd, "returncode": r.returncode, "kind": "build"})
        save()
        assert r.returncode == 0, r.stdout + r.stderr
        return exe

    def run(exe, *args):
        r = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=60)
        (out / ("run-%04d.log" % len(report["commands"]))).write_text(r.stdout + r.stderr, encoding="utf-8")
        report["commands"].append({"command": [str(exe), *args], "returncode": r.returncode, "kind": "run"})
        save()
        return r

    def stops(r, message):
        return (r.returncode not in (0, 1) and FAIL_STOP + message in r.stderr
                and "FAIL:" not in r.stderr)

    STOPS = [
        ("removed-prefetch", "GPU fence (device removed) failed"),
        ("removed-refresh", "GPU fence (device removed) failed"),
        ("removed-coherent", "s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device) failed"),
        ("removed-while-waiting", "GPU fence (device removed) failed"),
        ("recording", "surface prefetch before command submission failed"),
        ("removed-stale", "GPU fence (device removed) failed"),
        ("removed-land", "GPU fence (device removed) failed"),
    ]
    BAD_PREFETCH = ["zz", "1,2,3,4,5", "100000000", "1,", ",1", "1,,2", "1F00,1f00", "-1", " 1", "0x1F00", "1F00 "]
    BAD_STALE = ["zz", "1,2", "100000000", "1,", "1,,", "-1", " 1", "1F00 ", "0x1F00"]
    QUIET = ["unset", "first-in-flight", "not-prefetched", "stale-asks"]
    COUNTERS = "[CPUSURFACE_STALE] offset=00001F00 stale=5 fresh=2 copies=4"
    def full_pass(exe):
        r = run(exe)
        if r.returncode != 0 or "PASS" not in r.stdout or "FAIL:" in r.stderr:
            return "scenario: " + (r.stderr.strip().splitlines() or ["exit %d" % r.returncode])[-1]
        if COUNTERS not in r.stderr:
            return "counters: " + (r.stderr.strip().splitlines() or ["none"])[-1]
        for mode in QUIET:
            r = run(exe, mode)
            if r.returncode != 0 or "FAIL:" in r.stderr:
                return mode + ": " + (r.stderr.strip().splitlines() or ["exit %d" % r.returncode])[-1]
            if mode == "unset" and "[CPUSURFACE_STALE]" in r.stderr:
                return "unset: the switch printed a counter"
            if mode == "not-prefetched" and "is not in RSX_CPU_SURFACE_PREFETCH" not in r.stderr:
                return "not-prefetched: no message"
        for mode, message in STOPS:
            if not stops(run(exe, mode), message):
                return "stop " + mode
        for bad in BAD_PREFETCH:
            if not stops(run(exe, "config", bad), "RSX_CPU_SURFACE_PREFETCH"):
                return "prefetch config %r accepted" % bad
        for bad in BAD_STALE:
            if not stops(run(exe, "stale-config", bad), "RSX_CPU_SURFACE_STALE"):
                return "stale config %r accepted" % bad
        return None

    exe = build("actual", region)
    failure = full_pass(exe)
    assert failure is None, failure
    print(run(exe).stdout.strip())
    print("PASS cases: unset, first read, fresh copy complete, fresh copy in flight, copy lands, "
          "layout change, another offset; counters " + COUNTERS.split("] ")[1])
    print("PASS stops: " + ", ".join(m for m, _ in STOPS) + "; "
          + str(len(BAD_PREFETCH)) + " malformed prefetch lists and "
          + str(len(BAD_STALE)) + " malformed stale offsets rejected")
    serve = "        if(cpu_surface_stale_serves(r))return cpu_surface_stale_serve();\n"
    permit = "            if(cpu_surface_view_exchange(&v->stale,0)!=1)return 0;\n"
    mutants = [
        ("stale-read-advances-generation", serve,
         "        if(cpu_surface_stale_serves(r)){r->cpu_generation=r->gpu_generation;return cpu_surface_stale_serve();}\n"),
        ("stale-read-blocks", serve, ""),
        ("stale-read-without-landed-copy",
         "return s->configured && s->landed && s->off==r->off", "return s->configured && s->off==r->off"),
        ("stale-read-after-layout-change",
         "r->cpu_address==s->address &&\n        r->cpu_pitch==s->pitch && r->w==s->w && r->h==s->height && r->cpu_format==s->format;",
         "r->cpu_address;"),
        ("client-stale-read-unlimited", permit, ""),
        ("client-stale-read-never", permit, "            return 0;\n"),
        ("client-stale-read-does-not-ask",
         "            if(cpu_surface_view_exchange(&v->stale,0)!=1)return 0;\n"
         "            if(!cpu_surface_view_load(&v->noted))cpu_surface_view_store(&v->noted,1);\n",
         "            if(cpu_surface_view_exchange(&v->stale,0)!=1)return 0;\n"),
        ("poll-never-lands", "        if(r && cpu_surface_stale_listed(r))cpu_surface_stale_land(e,r);\n", ""),
        ("poll-lands-in-flight-copy",
         "    if(!cpu_surface_fence_reached(s_cpu_surface_prefetch_fence,e->fence))return;\n", ""),
        ("poll-lands-older-copy",
         "    if(!e->generation || e->generation<=r->cpu_generation)return;\n", "    if(!e->generation)return;\n"),
        ("switch-unset-counts",
         "    if(cpu_surface_stale_listed(r)){s_cpu_surface_stale.copies++;cpu_surface_stale_landed(r);}\n",
         "    {s_cpu_surface_stale.copies++;cpu_surface_stale_landed(r);}\n"),
        ("retire-keeps-landed-copy",
         "    else if(cpu_surface_stale_listed(r))s_cpu_surface_stale.landed=0;\n", ""),
        ("stale-list-accepts-more-offsets",
         '    if(!digits || *list)cpu_surface_fail("RSX_CPU_SURFACE_STALE (one hex offset)",E_INVALIDARG);\n',
         '    if(!digits)cpu_surface_fail("RSX_CPU_SURFACE_STALE (one hex offset)",E_INVALIDARG);\n'),
        ("stale-offset-outside-prefetch-list", "        if(!listed) {\n", "        if(0) {\n"),
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
