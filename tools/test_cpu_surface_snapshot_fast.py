"""Compile the production snapshot fast path against the authored model of the surface prefetch fixture.

Checked: a client thread serves a snapshot request only when no recorded draw targets a range and
each range lies in one current surface or in no surface, the second check after the read, the
declined request on the boundary path, the profile record, the two-thread invariant, the wiring,
and mutants."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
START = "typedef struct CpuSurfaceRequest {"
PROFILE = "static int s_cpu_surface_profile;"
FENCE = "static void cpu_surface_fence_wait(void)"
VIEWS = "/* Renderer thread only: one client-visible view"
PREFETCH = "/* Prefetched copies of opted-in surfaces"
FAST = "/* Snapshot fast path, client thread."
EAGER = "/* Eager submission after writes to listed surfaces."
PROMPT = "/* Prompt service between batches."
INIT = "static void cpu_surface_init"
RETIRE = "static void cpu_surface_retire"
IDENTITY = ("if((LONG)GetCurrentThreadId()==InterlockedCompareExchange(&s_cpu_surface_thread,0,0))\n"
            "        return VM_READ_SNAPSHOT_UNAVAILABLE;")
CALL = "if(cpu_surface_snapshot_fast(ranges,count,staging,output_bytes,(u32)GetCurrentThreadId()))return VM_READ_SNAPSHOT_OK;"
QUEUE = "    CpuSurfaceRequest request={0};"
WAIT = "    DWORD wait=WaitForSingleObject(request.done,30000);"
SWITCH = ("const char *snapshot_fast=getenv(\"RSX_CPU_SURFACE_SNAPSHOT_FAST\");"
          "s_cpu_surface_snapshot_fast=snapshot_fast&&snapshot_fast[0]=='1'&&!snapshot_fast[1];")
HANDLER = "vm_read_snapshot_set_handler(cpu_surface_snapshot_request,NULL);"
BOUNDARY = "if(cpu_surface_snapshot_pending())(void)cellGcm_with_fifo_boundary(cpu_surface_snapshot_boundary,NULL);"
CAPTURE = "cpu_surface_pending_add(draw->cpu_address[i],draw->cpu_pitch[i],draw->rt_h)"
RESET = "    s_cpu_surface_pending_count=0;s_cpu_surface_pending_overflow=0;\n"
STEP = ("cpu_surface_view_store(&s_cpu_surface_pending_sequence,"
        "cpu_surface_view_load(&s_cpu_surface_pending_sequence)+1);")
SWITCH_OFF = "    if(!s_cpu_surface_snapshot_fast)return 0;\n"
CLEAR = "    if(!cpu_surface_snapshot_clear(ranges,count,seen))goto queued;\n"
READ = ("    if(!ReadProcessMemory(GetCurrentProcess(),vm_base+ranges[i].address,bytes+ranges[i].output_offset,"
        "ranges[i].bytes,&copied) || copied!=ranges[i].bytes)goto queued;\n")
RECHECK = "    if(!cpu_surface_snapshot_recheck(ranges,count,seen))goto queued;\n"
COMMIT = "    memcpy(staging,bytes,output_bytes);\n"
RECORD = "cpu_surface_profile_emit(&fast);"
FAST_LINE = re.compile(r"^FAST address=([0-9A-F]{8}) bytes=(\d+) hits=(\d+)$", re.M)


def function(source, name):
    """One production function, from its definition to its closing brace."""
    match = re.search(r"static (?:void|int) " + name + r"\([^;)]*\)\s*\{", source)
    assert match, "missing production function " + name
    depth, end = 1, match.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def in_function(text, name, before, after):
    """One replacement inside one production function of text."""
    body = function(text, name)
    assert body.count(before) == 1, (name, before)
    return text.replace(body, body.replace(before, after))


def wiring(source, fast, prompt, request, init, backend, capture):
    """Order and hooks that the model cannot run: the request entry, the switch and the backend."""
    assert source.count(CALL) == 1 and request.count(CALL) == 1, "a client thread tries the snapshot fast path once"
    assert request.index(IDENTITY) < request.index(CALL) < request.index(QUEUE), (
        "the renderer thread keeps its refusal; a client thread tries the fast path before it queues")
    assert init.count(SWITCH) == 1, "RSX_CPU_SURFACE_SNAPSHOT_FAST=1 enables the fast path; the default keeps the boundary path"
    assert init.count(HANDLER) == 1, "one snapshot handler"
    body = function(fast, "cpu_surface_snapshot_fast")
    order = [body.index(SWITCH_OFF), body.index(CLEAR), body.index(READ), body.index(RECHECK), body.index(COMMIT)]
    assert order == sorted(order) and body.index(COMMIT) < body.index(RECORD) < body.index("    return 1;\n"), (
        "the client checks, reads into its own buffer, checks again, commits, then writes its record")
    add = function(prompt, "cpu_surface_pending_add")
    assert add.count(STEP) == 2 and add.index(STEP) < add.index("cpu_surface_pending_insert(") < add.rindex(STEP), (
        "the renderer publishes each recorded draw between two steps of the sequence number")
    reset = function(prompt, "cpu_surface_batch_submitted")
    assert reset.count(STEP) == 2 and reset.index(STEP) < reset.index(RESET) < reset.rindex(STEP), (
        "the renderer publishes the submitted batch between two steps of the sequence number")
    clear = function(prompt, "cpu_surface_snapshot_clear")
    assert (clear.index("cpu_surface_view_load(&v->ready)") < clear.index("cpu_surface_pending_overlaps(")
            < clear.index("cpu_surface_view_load(&s_cpu_surface_pending_sequence)!=pending")), (
        "the range check reads the surfaces, then the recorded draws with their sequence number")
    assert backend.count(BOUNDARY) == 1, "the pump keeps the FIFO boundary for queued snapshot requests"
    assert CAPTURE in capture and "cpu_surface_service_ready();" in capture, (
        "each recorded draw registers its targets before the prompt service")


def record_failure(run):
    """The profile record of each served request: no queue, no service, the surfaces counted."""
    expected = [(int(a, 16), int(b), int(h)) for a, b, h in FAST_LINE.findall(run.stdout)]
    records = [line for line in run.stderr.splitlines() if "[CPUSURFACE_PROFILE" in line]
    if len(records) != len(expected):
        return "%d records for %d served requests" % (len(records), len(expected))
    for line, (address, bytes_, hits) in zip(records, expected):
        field = dict(token.split("=") for token in line.split()[1:] if "=" in token)
        if (int(field["address"], 16), int(field["bytes"])) != (address, bytes_):
            return "record of %08X/%u is missing" % (address, bytes_)
        zero = [key for key in ("queue", "service", "return", "copies") if int(field[key]) != 0]
        counted = [key for key in ("matched", "hits") if int(field[key]) != hits]
        if zero or counted:
            return "record of %08X/%u: %s" % (
                address,
                bytes_,
                ", ".join("%s=%s" % (key, field[key]) for key in zero + counted),
            )
        if int(field["client"]) != 7:
            return "record of %08X/%u names the wrong client" % (address, bytes_)
    return None


# (name, where, [(before, after), ...])
MUTANTS = [
    ("serves-pending-draw", "prompt",
     [("    for(u32 j=0;j<count;j++)if(cpu_surface_pending_overlaps(ranges[j].address,ranges[j].bytes))return 0;\n", "")]),
    ("serves-superseded-generation", "prompt",
     [("if(!cpu_surface_view_load(&v->ready) || first<begin || last>end || owners[j]++)return 0;",
       "if(first<begin || last>end || owners[j]++)return 0;")]),
    ("serves-partial-range", "prompt",
     [("if(!cpu_surface_view_load(&v->ready) || first<begin || last>end || owners[j]++)return 0;",
       "if(!cpu_surface_view_load(&v->ready) || owners[j]++)return 0;")]),
    ("serves-two-surfaces", "prompt",
     [("if(!cpu_surface_view_load(&v->ready) || first<begin || last>end || owners[j]++)return 0;",
       "if(!cpu_surface_view_load(&v->ready) || first<begin || last>end)return 0;")]),
    ("serves-during-update", "prompt",
     [("        if(sequence&1)return 0;\n        int holds=0;\n", "        int holds=0;\n")]),
    ("serves-during-list-change", "prompt", [("    if(pending&1)return 0;\n", "")]),
    ("adjacent-below-overlaps", "prompt", [("if(first>=end || last<=begin)continue;", "if(first>=end || last<begin)continue;")]),
    ("adjacent-above-overlaps", "prompt", [("if(first>=end || last<=begin)continue;", "if(first>end || last<=begin)continue;")]),
    ("ignores-view-change", "prompt",
     [("now[CPU_SURFACE_SNAPSHOT_SEQUENCES]==seen[CPU_SURFACE_SNAPSHOT_SEQUENCES] && now[CPU_SURFACE_SNAPSHOT_PENDING]==seen[CPU_SURFACE_SNAPSHOT_PENDING];",
       "now[CPU_SURFACE_SNAPSHOT_PENDING]==seen[CPU_SURFACE_SNAPSHOT_PENDING];")]),
    ("ignores-draw-change", "prompt", [(" && now[CPU_SURFACE_SNAPSHOT_PENDING]==seen[CPU_SURFACE_SNAPSHOT_PENDING];", ";")]),
    ("ignores-table-change", "prompt",
     [("    return cpu_surface_snapshot_clear(ranges,count,now) && now[CPU_SURFACE_SNAPSHOT_TABLE]==seen[CPU_SURFACE_SNAPSHOT_TABLE] &&\n",
       "    return cpu_surface_snapshot_clear(ranges,count,now) &&\n")]),
    ("ignores-switch", "fast", [(SWITCH_OFF, "")]),
    ("no-recheck", "fast", [(RECHECK, "")]),
    ("wrong-byte-range", "fast",
     [("vm_base+ranges[i].address,bytes+ranges[i].output_offset", "vm_base+ranges[i].address+1,bytes+ranges[i].output_offset")]),
    ("wrong-output-offset", "fast", [("bytes+ranges[i].output_offset,ranges[i].bytes,&copied)", "bytes+i,ranges[i].bytes,&copied)")]),
    ("unreadable-served", "fast", [(" || copied!=ranges[i].bytes)goto queued;", " || copied!=ranges[i].bytes)continue;")]),
    ("fast-record-measures-wait", "fast",
     [("fast.profile.queued=fast.profile.started=fast.profile.finished=fast.profile.returned=cpu_surface_tick();",
       "fast.profile.queued=cpu_surface_tick();fast.profile.started=fast.profile.finished=fast.profile.returned=cpu_surface_tick();")]),
    ("fast-record-wrong-hits", "fast",
     [("fast.profile.matched=fast.profile.hits=seen[CPU_SURFACE_SNAPSHOT_MATCHED];", "fast.profile.matched=fast.profile.hits=1;")]),
]
# Mutants of order and hooks that only the wiring check sees.
WIRING_MUTANTS = [
    ("request-without-fast-path", {"request": lambda text: text.replace(CALL, "")}),
    ("fast-path-after-queue", {"request": lambda text: text.replace(CALL + "\n", "").replace(WAIT, CALL + "\n" + WAIT)}),
    ("identity-check-after-fast-path",
     {"request": lambda text: text.replace(IDENTITY + "\n", "").replace(QUEUE, IDENTITY + "\n" + QUEUE)}),
    ("switch-unread", {"init": lambda text: text.replace(SWITCH + "\n", "")}),
    ("switch-default-on",
     {"init": lambda text: text.replace(SWITCH, SWITCH.replace("snapshot_fast&&snapshot_fast[0]=='1'&&!snapshot_fast[1]",
                                                              "!(snapshot_fast&&snapshot_fast[0]=='0'&&!snapshot_fast[1])"))}),
    ("commit-before-recheck",
     {"fast": lambda text: in_function(text, "cpu_surface_snapshot_fast", RECHECK + COMMIT, COMMIT + RECHECK)}),
    ("recorded-draw-unpublished",
     {"prompt": lambda text: in_function(text, "cpu_surface_pending_add", "    " + STEP + "\n    cpu_surface_pending_insert(",
                                         "    cpu_surface_pending_insert(")}),
    ("batch-reset-unpublished",
     {"prompt": lambda text: in_function(text, "cpu_surface_batch_submitted", "    " + STEP + "\n" + RESET, RESET)}),
    ("boundary-removed", {"backend": lambda text: text.replace(BOUNDARY, "")}),
    ("capture-stops-registering", {"capture": lambda text: text.replace(CAPTURE, "(void)0")}),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler")
    parser.add_argument("--output", type=Path, help="folder for the composed programs and result.json")
    args = parser.parse_args()
    source_path = ROOT / "libs/video/rsx_cpu_surface.inc"
    source = source_path.read_text(encoding="utf-8")
    backend_path = ROOT / "libs/video/rsx_d3d12_backend.c"
    backend = backend_path.read_text(encoding="utf-8")
    fixture_path = ROOT / "libs/video/tests/test_cpu_surface_snapshot_fast.c"
    fixture = fixture_path.read_text(encoding="utf-8")
    prefetch_path = ROOT / "libs/video/tests/test_cpu_surface_prefetch.c"
    prefetch = prefetch_path.read_text(encoding="utf-8")
    for marker in ("/* PREFETCH_MODEL */", "/* PRODUCTION_DECLARATIONS */", "/* PRODUCTION_HELPERS */",
                   "/* PRODUCTION_VIEWS */", "/* PRODUCTION_HOOKS */", "/* PRODUCTION_SNAPSHOT_FAST */",
                   "/* PRODUCTION_PROMPT */"):
        assert fixture.count(marker) == 1, marker
    assert prefetch.count("/* PRODUCTION */") == 1
    model = prefetch[: prefetch.index("/* PRODUCTION */")]
    for marker in (START, PROFILE, FENCE, VIEWS, PREFETCH, FAST, EAGER, PROMPT, INIT, RETIRE):
        assert source.count(marker) == 1, marker
    order = [source.index(marker) for marker in (START, PROFILE, FENCE, VIEWS, PREFETCH, FAST, EAGER, PROMPT, INIT, RETIRE)]
    assert order == sorted(order), "the production blocks moved"
    request = function(source, "cpu_surface_snapshot_request")
    blocks = {
        "declarations": source[source.index(START): source.index(PROFILE)],
        "helpers": source[source.index(PROFILE): source.index(FENCE)],
        "views": source[source.index(VIEWS): source.index(PREFETCH)],
        "hooks": "\n".join(function(source, name) for name in ("cpu_surface_bind", "cpu_surface_written")),
        "fast": source[source.index(FAST): source.index(EAGER)],
        "prompt": source[source.index(PROMPT): source.index(INIT)],
    }
    init = source[source.index(INIT): source.index(RETIRE)]
    assert request in blocks["fast"] and SWITCH in init

    out = ROOT / "build/cpu-surface-snapshot-fast-tests"
    out.mkdir(parents=True, exist_ok=True)
    report = {
        "qualification": "Production bodies against the authored model of the surface prefetch fixture with the real guest_read_sync transport, plus two real threads. Actual D3D12 coverage runs separately on Windows.",
        "source_sha256": {
            str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (source_path, backend_path, fixture_path, prefetch_path, Path(__file__))
        },
        "commands": [],
        "mutants_rejected": [],
        "wiring_mutants_rejected": [],
    }

    def save():
        (out / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")

    compiler = (
        args.compiler
        or os.environ.get("MV_CLANG")
        or os.environ.get("CC")
        or shutil.which("clang-cl")
        or shutil.which("clang")
        or shutil.which("cc")
    )
    assert compiler, "No C compiler found; pass --compiler"
    msvc = Path(compiler).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
    video, include = ROOT / "libs/video", ROOT / "include"

    def check(name, changed):
        """None when the composed program passes, else the reason it fails."""
        texts = dict(blocks, **changed)
        folder = out / name
        folder.mkdir(exist_ok=True)
        c = folder / "check.c"
        text = fixture
        for marker, value in (("/* PREFETCH_MODEL */", model), ("/* PRODUCTION_DECLARATIONS */", texts["declarations"]),
                              ("/* PRODUCTION_HELPERS */", texts["helpers"]), ("/* PRODUCTION_VIEWS */", texts["views"]),
                              ("/* PRODUCTION_HOOKS */", texts["hooks"]), ("/* PRODUCTION_SNAPSHOT_FAST */", texts["fast"]),
                              ("/* PRODUCTION_PROMPT */", texts["prompt"])):
            text = text.replace(marker, value)
        c.write_text(text, encoding="utf-8")
        exe = folder / ("check.exe" if os.name == "nt" else "check")
        sync = ROOT / "runtime/memory/guest_read_sync.c"
        if msvc:
            cmd = [compiler, "/nologo", "/O2", "/UNDEBUG", "/std:c11", "/I" + str(video), "/I" + str(include),
                   str(c), str(sync), "/Fo" + str(folder) + os.sep, "/Fe" + str(exe)]
        else:
            cmd = [compiler, "-std=c11", "-O2", "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-function", "-Wno-unused-parameter", "-I" + str(video), "-I" + str(include),
                   str(c), str(sync), "-o", str(exe), "-pthread"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        (folder / "build.log").write_text(r.stdout + r.stderr, encoding="utf-8")
        report["commands"].append({"name": name, "command": cmd, "returncode": r.returncode, "kind": "build"})
        save()
        if r.returncode:
            return "build failed: " + r.stderr.strip()[-300:]
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
        (folder / "run.log").write_text(r.stdout + r.stderr, encoding="utf-8")
        report["commands"].append({"name": name, "returncode": r.returncode, "kind": "run"})
        save()
        if r.returncode:
            lines = [line for line in r.stderr.splitlines() if line.startswith("FAIL")]
            return "exit %d: %s" % (r.returncode, lines[-1] if lines else r.stderr.strip()[-200:])
        if "PASS:" not in r.stdout:
            return "no PASS line"
        return record_failure(r)

    def run(name):
        return subprocess.run([str(out / name / ("check.exe" if os.name == "nt" else "check"))],
                              capture_output=True, text=True, timeout=300)

    capture = source[source.index("static void cpu_surface_capture"):]
    wiring(source, blocks["fast"], blocks["prompt"], request, init, backend, capture)
    print("PASS wiring: renderer refusal first, fast path before the queue, switch, one handler, "
          "published recorded draws, pump boundary, commit order")

    failure = check("actual", {})
    assert failure is None, "actual: " + failure
    print(run("actual").stdout.strip())
    print("PASS: records of served requests: queue=0 service=0 return=0")

    for name, where, changes in MUTANTS:
        text = blocks[where]
        for before, after in changes:
            assert text.count(before) == 1, (name, before)
            text = text.replace(before, after)
        failure = check(name, {where: text})
        assert failure is not None, "mutant survived: " + name
        report["mutants_rejected"].append(name)
        save()
        print("PASS mutant " + name + " rejected: " + failure)

    for name, changes in WIRING_MUTANTS:
        texts = {"source": source, "fast": blocks["fast"], "prompt": blocks["prompt"], "request": request,
                 "init": init, "backend": backend, "capture": capture}
        for where, change in changes.items():
            changed = change(texts[where])
            assert changed != texts[where], (name, where)
            texts[where] = changed
        try:
            wiring(texts["source"], texts["fast"], texts["prompt"], texts["request"], texts["init"], texts["backend"], texts["capture"])
        except AssertionError as exc:
            report["wiring_mutants_rejected"].append(name)
            save()
            print("PASS wiring mutant " + name + " rejected: " + str(exc))
            continue
        raise AssertionError("wiring mutant survived: " + name)

    report["passed"] = True
    save()
    print("PASS: snapshot fast path, range check, published recorded draws and wiring ("
          + str(len(MUTANTS)) + " mutants and " + str(len(WIRING_MUTANTS)) + " wiring mutants rejected)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
