"""Compile the production fast CPU surface read against the authored GPU model of the surface prefetch fixture.

Checked: a client thread serves only a read whose surfaces are current, a read during a
renderer update queues, the profile record, the prefetch note, the two-thread view
invariant, the wiring, and mutants."""

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
STOP = "static void cpu_surface_snapshot_trace("
TAIL = ("cpu_surface_bind", "cpu_surface_written", "cpu_surface_retire")
RECHECK = "if(cpu_surface_view_load(&v->sequence)!=sequence)return CPU_SURFACE_VIEW_CHANGING;"
CALL = "if(cpu_surface_fast_request(address,bytes,client_thread))return;"
SWITCH = "const char *fast=getenv(\"RSX_CPU_SURFACE_FAST\");s_cpu_surface_fast=fast&&fast[0]=='1'&&!fast[1];"
NOTE = "            if(!cpu_surface_view_load(&v->noted))cpu_surface_view_store(&v->noted,1);\n"
REFRESH_NOTE = "if(view && cpu_surface_view_exchange(&view->noted,0))e->wanted=1;"
REFRESH_VIEW = "        CpuSurfaceView *view=r?cpu_surface_view_entry(cpu_surface_view_index(r)):NULL;\n"
RECORD = "fast.profile.matched=fast.profile.hits=hits;cpu_surface_profile_emit(&fast);"
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


def wiring(source, region, request, init, backend):
    """Order and hooks that the model cannot run: the request entry and the backend."""
    assert source.count(CALL) == 1 and request.count(CALL) == 1, "a client thread tries the fast path once"
    assert (
        request.index("cpu_surface_read(address,bytes);s_cpu_surface_profile_active=previous;")
        < request.index(CALL)
        < request.index("request.done=CreateEventA(")
    ), "the renderer thread keeps its direct path; a client thread tries the fast path before it queues"
    assert init.count(SWITCH) == 1, "only RSX_CPU_SURFACE_FAST=1 enables caller-side reads"
    assert init.count("vm_read_sync_set_handler(cpu_surface_request,NULL);") == 1, "one coherent read handler"
    assert region.count(RECHECK) == 1, "a client uses a view only when its sequence number did not change"
    for name in ("cpu_surface_copy", "cpu_surface_prefetch_take"):
        body = function(region, name)
        assert body.index("rsx_surface_readback_rows(") < body.index("cpu_surface_view_update(r);"), (
            name + " publishes a copy after it packed the bytes"
        )
    for hook in ("cpu_surface_bind(", "cpu_surface_written(", "cpu_surface_retire("):
        assert backend.count(hook) >= 1, "the renderer reaches " + hook[:-1] + " through the backend"
    written = function(source, "cpu_surface_written")
    assert written.count("gpu_generation++;cpu_surface_view_update(") == 2, "each recorded write publishes its surface"
    assert "cpu_surface_view_update(r);" in function(source, "cpu_surface_bind"), "a bound surface is published"


def record_failure(run):
    """The profile record of each served read: no queue, no service, the served surfaces counted."""
    expected = [(int(a, 16), int(b), int(h)) for a, b, h in FAST_LINE.findall(run.stdout)]
    records = [line for line in run.stderr.splitlines() if "[CPUSURFACE_PROFILE" in line]
    if len(records) != len(expected):
        return "%d records for %d served reads" % (len(records), len(expected))
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
    ("serves-superseded-generation", "region",
     [("return ready?CPU_SURFACE_VIEW_READY:CPU_SURFACE_VIEW_PENDING;", "return ready?CPU_SURFACE_VIEW_READY:CPU_SURFACE_VIEW_READY;")]),
    ("serves-during-update", "region",
     [("    D3D12_RANGE range={0,(SIZE_T)bytes};void *mapped=NULL;\n",
       "    r->cpu_generation=r->gpu_generation;cpu_surface_view_update(r);\n"
       "    D3D12_RANGE range={0,(SIZE_T)bytes};void *mapped=NULL;\n")]),
    ("serves-during-prefetch-update", "region",
     [("    D3D12_RANGE range={0,(SIZE_T)e->readback_bytes};void *mapped=NULL;\n",
       "    r->cpu_generation=r->gpu_generation;cpu_surface_view_update(r);\n"
       "    D3D12_RANGE range={0,(SIZE_T)e->readback_bytes};void *mapped=NULL;\n")]),
    ("copy-never-publishes", "region",
     [("    r->cpu_generation=r->gpu_generation;cpu_surface_view_update(r);\n    if(s_cpu_surface_profile_active) {",
       "    r->cpu_generation=r->gpu_generation;\n    if(s_cpu_surface_profile_active) {")]),
    ("prefetch-never-publishes", "region",
     [("e->used++;cpu_surface_view_update(r);", "e->used++;")]),
    ("adjacent-below-overlaps", "region", [("(u64)address+bytes<=begin", "(u64)address+bytes<begin")]),
    ("adjacent-above-overlaps", "region", [("if((u64)address>=end || ", "if((u64)address>end || ")]),
    ("ignores-later-surface", "region",
     [("            hits++;\n        }", "            hits++;break;\n        }")]),
    ("serves-unowned-read", "region", [("    if(!hits)return 0;\n", "")]),
    ("ignores-switch", "region", [("    if(!s_cpu_surface_fast)return 0;\n", "")]),
    ("odd-sequence-served", "region",
     [("    if(sequence&1)return CPU_SURFACE_VIEW_CHANGING;\n", "")]),
    ("fast-read-never-noted", "region", [(NOTE, "")]),
    ("refresh-ignores-note", "region", [(REFRESH_VIEW + "        " + REFRESH_NOTE + "\n", "")]),
    ("refresh-leaves-note", "region",
     [(REFRESH_NOTE, "if(view && cpu_surface_view_load(&view->noted))e->wanted=1;")]),
    ("retire-keeps-note", "region", [("    if(v)cpu_surface_view_exchange(&v->noted,0);\n", "    (void)v;\n")]),
    ("fast-record-missing", "region",
     [(RECORD, "fast.profile.matched=fast.profile.hits=hits;")]),
    ("fast-record-wrong-hits", "region",
     [(RECORD, "fast.profile.matched=fast.profile.hits=1;cpu_surface_profile_emit(&fast);")]),
    ("fast-record-measures-wait", "region",
     [("fast.profile.queued=fast.profile.started=fast.profile.finished=fast.profile.returned=cpu_surface_tick();",
       "fast.profile.queued=cpu_surface_tick();fast.profile.started=fast.profile.finished=fast.profile.returned=cpu_surface_tick();")]),
    ("write-keeps-view-ready", "tail",
     [(";cpu_surface_view_update(&s_d3d.off_rt[primary]);}", ";}"),
      (";cpu_surface_view_update(&s_d3d.off_rt[secondary[i]]);}", ";}")]),
    ("bind-never-publishes", "tail",
     [("cpu_surface_fail(\"guest surface registration\",E_INVALIDARG);\n    cpu_surface_view_update(r);\n",
       "cpu_surface_fail(\"guest surface registration\",E_INVALIDARG);\n")]),
    ("retire-keeps-view", "tail",
     [("r->gpu_generation=r->cpu_generation=0;cpu_surface_view_retire(r);", "r->gpu_generation=r->cpu_generation=0;")]),
    ("growth-drops-views", "region",
     [("        cpu_surface_view_store(&table[i].begin,cpu_surface_view_load(&old[i].begin));\n"
       "        cpu_surface_view_store(&table[i].end,cpu_surface_view_load(&old[i].end));\n"
       "        cpu_surface_view_store(&table[i].ready,cpu_surface_view_load(&old[i].ready));\n", "")]),
    ("growth-drops-notes", "region",
     [("        cpu_surface_view_store(&table[i].noted,cpu_surface_view_load(&old[i].noted));\n", "")]),
    ("replaced-table-serves", "region",
     [("    for(u64 i=0;i<count;i++)cpu_surface_view_store(&old[i].sequence,cpu_surface_view_load(&old[i].sequence)+1);\n",
       "")]),
    ("growth-keeps-count", "region",
     [("(uintptr_t)table|next<<CPU_SURFACE_VIEW_COUNT", "(uintptr_t)table|count<<CPU_SURFACE_VIEW_COUNT")]),
]
# Mutants of order and hooks that only the wiring check sees.
def in_function(text, name, before, after):
    """One replacement inside one production function of text."""
    body = function(text, name)
    assert body.count(before) == 1, (name, before)
    return text.replace(body, body.replace(before, after))


WIRING_MUTANTS = [
    ("request-without-fast-path", {"request": lambda text: text.replace(CALL, "")}),
    ("request-queues-first", {"request": lambda text: text.replace(CALL + "\n", "").replace(
        "    request.done=CreateEventA(NULL,FALSE,FALSE,NULL);\n",
        "    request.done=CreateEventA(NULL,FALSE,FALSE,NULL);\n    " + CALL + "\n")}),
    ("switch-unread", {"init": lambda text: text.replace(SWITCH + "\n", "")}),
    ("changed-sequence-served", {"region": lambda text: text.replace("    " + RECHECK + "\n", "")}),
    ("copy-publishes-before-pack", {"region": lambda text: in_function(
        text, "cpu_surface_copy", "    D3D12_RANGE range={0,(SIZE_T)bytes};void *mapped=NULL;\n",
        "    cpu_surface_view_update(r);\n    D3D12_RANGE range={0,(SIZE_T)bytes};void *mapped=NULL;\n")}),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler")
    args = parser.parse_args()
    source_path = ROOT / "libs/video/rsx_cpu_surface.inc"
    source = source_path.read_text(encoding="utf-8")
    backend_path = ROOT / "libs/video/rsx_d3d12_backend.c"
    backend = backend_path.read_text(encoding="utf-8")
    fixture_path = ROOT / "libs/video/tests/test_cpu_surface_fast_read.c"
    fixture = fixture_path.read_text(encoding="utf-8")
    prefetch_path = ROOT / "libs/video/tests/test_cpu_surface_prefetch.c"
    prefetch = prefetch_path.read_text(encoding="utf-8")
    for marker in ("/* PREFETCH_MODEL */", "/* PRODUCTION_REGION */", "/* PRODUCTION_TAIL */",
                   "/* PREFETCH_HELPERS */"):
        assert fixture.count(marker) == 1, marker
    assert prefetch.count("/* PRODUCTION */") == 1
    model = prefetch[: prefetch.index("/* PRODUCTION */")]
    helpers = prefetch[prefetch.index("/* ---- Scenario helpers ---- */"): prefetch.index("int main(int argc")]
    assert "static OffRT *surface(" in helpers and "static void boot(" in helpers
    for marker in (START, STOP):
        assert source.count(marker) == 1, marker
    region = source[source.index(START): source.index(STOP)]
    tail = "\n".join(function(source, name) for name in TAIL)
    request = function(source, "cpu_surface_request")
    init = source[source.index("static void cpu_surface_init"): source.index("static void cpu_surface_retire")]

    out = ROOT / "build/cpu-surface-fast-read-tests"
    out.mkdir(parents=True, exist_ok=True)
    report = {
        "qualification": "Production bodies against the authored GPU model of the prefetch fixture, plus two real threads. Actual D3D12 coverage runs separately on Windows.",
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

    def build(name, region_text, tail_text):
        folder = out / name
        folder.mkdir(exist_ok=True)
        c = folder / "check.c"
        text = fixture
        for marker, value in (("/* PREFETCH_MODEL */", model), ("/* PRODUCTION_REGION */", region_text),
                              ("/* PRODUCTION_TAIL */", tail_text), ("/* PREFETCH_HELPERS */", helpers)):
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
        assert r.returncode == 0, name + ": " + r.stdout + r.stderr
        return exe

    def run(exe):
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
        return r

    def check(name, region_text, tail_text):
        """None when the composed program passes, else the reason it fails."""
        exe = build(name, region_text, tail_text)
        r = run(exe)
        (exe.parent / "run.log").write_text(r.stdout + r.stderr, encoding="utf-8")
        report["commands"].append({"name": name, "returncode": r.returncode, "kind": "run"})
        save()
        if r.returncode:
            lines = [line for line in r.stderr.splitlines() if line.startswith("FAIL")]
            return "exit %d: %s" % (r.returncode, lines[-1] if lines else r.stderr.strip()[-200:])
        if "PASS:" not in r.stdout:
            return "no PASS line"
        return record_failure(r)

    wiring(source, region, request, init, backend)
    print("PASS wiring: direct path, fast path before the queue, switch, one handler, backend hooks")

    failure = check("actual", region, tail)
    assert failure is None, "actual: " + failure
    r = run(out / "actual" / ("check.exe" if os.name == "nt" else "check"))
    print(r.stdout.strip())
    print("PASS: records of served reads: queue=0 service=0 return=0")

    for name, where, changes in MUTANTS:
        texts = {"region": region, "tail": tail}
        for before, after in changes:
            assert texts[where].count(before) == 1, (name, before)
            texts[where] = texts[where].replace(before, after)
        failure = check(name, texts["region"], texts["tail"])
        assert failure is not None, "mutant survived: " + name
        report["mutants_rejected"].append(name)
        save()
        print("PASS mutant " + name + " rejected: " + failure)

    for name, changes in WIRING_MUTANTS:
        texts = {"source": source, "region": region, "request": request, "init": init, "backend": backend}
        for where, change in changes.items():
            changed = change(texts[where])
            assert changed != texts[where], (name, where)
            texts[where] = changed
        try:
            wiring(texts["source"], texts["region"], texts["request"], texts["init"], texts["backend"])
        except AssertionError as exc:
            report["wiring_mutants_rejected"].append(name)
            save()
            print("PASS wiring mutant " + name + " rejected: " + str(exc))
            continue
        raise AssertionError("wiring mutant survived: " + name)

    report["passed"] = True
    save()
    print(
        "PASS: fast CPU surface read, view publication, prefetch note and wiring ("
        + str(len(MUTANTS)) + " mutants and " + str(len(WIRING_MUTANTS)) + " wiring mutants rejected)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
