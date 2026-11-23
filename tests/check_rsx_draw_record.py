#!/usr/bin/env python3
"""Compile the production per-draw GPU record block with fake D3D12 objects,
and audit its call sites in the backend.

The record starts with RSX_PHASE_GPU_DRAWS=1 and a RSX_PHASE_CSV path, like
the record of the game copy of this backend, and writes rsx_draws.csv next to
that path. RSX_DRAW_CSV belongs to the live draw fingerprints (rsx_live_draw.c).

The fixture covers: off by default; a path alone and the switch alone start
nothing; no record without a GPU timestamp frequency; the rows of a batch
written at the start of the next batch; row facts, the state flags, the
barrier count, the time before the draw and the draw time; a row without an
end timestamp; a failed readback; the row limit with one log line; file close
and object release at shutdown; both path separators. Mutants of the backend
and of the compiled block must fail. No game input; no performance claim."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BEGIN = "/* RSX draw CSV."
END = "/* End of the RSX draw CSV block. */"
HEADER = ("batch,draw,verts,idx,prim,rt0,rt1,rt2,rt3,zeta,vp_w,vp_h,vs,vs_hash,fp,fp_snap,"
          "rt_chg,vp_chg,pso_chg,srv_chg,barriers,depth,gap_us,gpu_us")
EXPECTED = [
    HEADER,
    "0,3,12,0,4,01A80500,0000AB00,00000000,00000000,0,1280,720,2,11223344,036CC2C1,0,0,1,0,0,0,0,60.000,20.000",
    "0,4,300,0,4,00000000,00000000,00000000,00000000,0,320,180,-1,00000000,00000010,0,1,0,1,1,2,0,250.000,30.000",
    "2,20,6,0,4,00000000,00000000,00000000,00000000,0,0,0,7,00000000,00000000,0,0,0,0,0,0,0,10.000,5.000"]
LIMIT = 3
ROW = "phase_draw_row(dr, d, row_flags, s_rt_barriers - row_barriers);"
ROW_LINE = " " * 16 + ROW + "\n"
QUERY_LINE = " " * 16 + "phase_draw_query();\n"
COLLECT = "        phase_draw_collect();\n"
RESET = "    s_draw_rows_n = 0;\n"
SWITCH = '        if (!draws || strcmp(draws, "1")) return 0;\n'
CHECKS = {"off": 4, "noswitch": 2, "nopath": 2, "noclock": 5, "on": 42}


def balanced(text, at, pattern):
    """Text of the function or block in `text` that starts at `at`."""
    start, state, i = at, "code", text.index("{", at) + 1
    depth = 1
    while i < len(text):
        c, pair = text[i], text[i:i + 2]
        if state == "line":
            if c == "\n":
                state = "code"
        elif state == "block":
            if pair == "*/":
                state = "code"
                i += 1
        elif state in ('"', "'"):
            if c == "\\":
                i += 1
            elif c == state:
                state = "code"
        elif pair in ("//", "/*"):
            state = "line" if pair == "//" else "block"
            i += 1
        elif c in ('"', "'"):
            state = c
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if not depth:
                return text[start:i + 1]
        i += 1
    raise AssertionError("unterminated " + pattern)


def function(source, name):
    match = re.search(r"\b(?:static\s+)?(?:void|int|const char\*)\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{", source)
    assert match, "missing actual helper: " + name
    return balanced(source, match.start(), name)


def record_block(source):
    assert source.count(BEGIN) == 1 and source.count(END) == 1, "one draw CSV block required"
    return source[source.index(BEGIN):source.index(END)]


def contract(source):
    assert "RSX_DRAW_CSV" not in source, "RSX_DRAW_CSV is the live draw fingerprint switch of rsx_live_draw.c"
    block = record_block(source)
    assert 'getenv("RSX_PHASE_GPU_DRAWS")' in block and 'getenv("RSX_PHASE_CSV")' in block, \
        "the record takes the switch and the directory of the game copy"
    on = function(block, "phase_draw_csv_on")
    assert SWITCH in on, "the record needs the per-draw timestamp switch"
    assert "Draw CSV off: RSX_PHASE_CSV gives no path" in on, "the record says when the path is missing"
    path = function(block, "phase_draw_csv_path")
    assert 'memcpy(path + dir, "rsx_draws.csv", sizeof "rsx_draws.csv");' in path and \
        "strrchr(phases, '\\\\')" in path and "strrchr(phases, '/')" in path, \
        "the file is rsx_draws.csv in the directory of RSX_PHASE_CSV, with either separator"
    begin = function(block, "phase_draw_batch_begin")
    assert begin.count(COLLECT) == 1 and begin.index(COLLECT) < begin.index(RESET), \
        "the rows of the previous batch are written before the reset"
    assert "fclose(s_draw_csv);" in function(block, "phase_draw_shutdown"), "shutdown closes the file"
    transition = function(source, "off_rt_transition")
    assert transition.count("s_rt_barriers++;") == 1 and \
        transition.index("->ResourceBarrier(") < transition.index("s_rt_barriers++;"), \
        "the transition count follows the issued barrier"
    submit = function(source, "submit_recorded_commands")
    assert submit.count("phase_draw_batch_end();") == 1 and \
        submit.index("phase_draw_batch_end();") < submit.index("->Close(s_d3d.cmd_list)"), \
        "the batch end timestamp and the readback copy precede Close"
    backend = function(source, "rsx_d3d12_backend_shutdown")
    assert backend.count("phase_draw_shutdown();") == 1 and \
        backend.index("address_retire();") < backend.index("phase_draw_shutdown();"), \
        "the record shuts down after the GPU retirement"
    frame = function(source, "render_frame")
    command_reset = "s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);\n"
    batch_begin = "    phase_draw_batch_begin();\n"
    loop = "for (u32 d = 0; d < s_d3d.draw_count && d < MAX_DRAWS; d++) {"
    assert frame.count(batch_begin) == 1 and frame.count(loop) == 1
    assert frame.index(command_reset) < frame.index(batch_begin) < frame.index(loop), \
        "the batch begins after the command list reset and before the replay loop"
    body = frame[frame.index(loop):]
    first = "u32 row_flags = 0, row_barriers = s_rt_barriers;"
    assert body.count(first) == 1 and body.index(first) < body.index("int want  = dr->rt_off"), \
        "each record starts with its own flags and transition count"
    targets = "row_flags |= PHASE_DRAW_TARGETS;"
    assert body.count(targets) == 1 and body.index("OMSetRenderTargets(s_d3d.cmd_list, nrt, rh, FALSE, &dsv_handle);") \
        < body.index(targets) < body.index("phase_draw_viewport(vp.TopLeftX"), \
        "the target flag sits in the target change block"
    pso = "if (next_pso != draw_last_pso) { row_flags |= PHASE_DRAW_PSO; draw_last_pso = next_pso; }"
    assert body.count(pso) == 1 and body.index("SetPipelineState(s_d3d.cmd_list, next_pso);") < body.index(pso), \
        "the pipeline flag follows the pipeline state of this draw"
    srv = "if (gh.ptr != draw_last_srv) { row_flags |= PHASE_DRAW_SRV; draw_last_srv = gh.ptr; }"
    assert body.count(srv) == 1 and body.index(srv) < body.index("SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);"), \
        "the SRV flag precedes the SRV table of this draw"
    assert body.index("D3D12_RECT dsc = {") < body.index("phase_draw_viewport(dvp.TopLeftX") \
        < body.index("row_flags |= PHASE_DRAW_VIEWPORT;") < body.index("RSSetViewports(s_d3d.cmd_list, 1, &dvp);"), \
        "the viewport flag compares the values given to the command list"
    start = body.index(QUERY_LINE + ROW_LINE)
    draw = body.index("DrawInstanced(s_d3d.cmd_list,\n", start)
    assert body.count(QUERY_LINE) == 2 and body.count(ROW_LINE) == 1 \
        and start < draw < body.index(QUERY_LINE, draw), \
        "the timestamps enclose the draw, and the row follows the start timestamp"


def without(text, part, replacement=""):
    changed = text.replace(part, replacement, 1)
    assert changed != text, part
    return changed


def reset_first(source):
    """`source` with the row reset before the write: the defect of the first port."""
    begin = function(source, "phase_draw_batch_begin")
    moved = without(begin, RESET).replace(COLLECT, RESET + COLLECT, 1)
    return source.replace(begin, moved, 1)


def build(compiler, environment, directory, name, code):
    c, exe = directory / (name + ".c"), directory / (name + (".exe" if os.name == "nt" else ""))
    c.write_text(code)
    if Path(compiler).stem.lower() in ("cl", "clang-cl"):
        command = [compiler, "/nologo", "/W4", "/WX", "/std:c11", str(c), "/Fe" + str(exe)]
    else:
        command = [compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(c), "-o", str(exe)]
    built = subprocess.run(command, cwd=directory, env=environment, capture_output=True, text=True,
                           errors="replace", timeout=180)
    assert built.returncode == 0, built.stdout + built.stderr
    return exe


def run(exe, environment, mode, label):
    result = subprocess.run([str(exe), mode], env=environment, capture_output=True, text=True,
                            errors="replace", timeout=30)
    assert result.returncode == 0 and "FAIL" not in result.stderr, label + ": " + result.stderr
    assert "draw record checks=%d failures=0" % CHECKS[mode] in result.stdout, result.stdout
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "libs/video/rsx_d3d12_backend.c")
    args = parser.parse_args()
    source = args.source.read_text()
    contract(source)
    end_query = "dr->vb_byte_offset / 256, 0);\n" + QUERY_LINE
    mutants = [
        reset_first(source),
        without(source, ROW_LINE),
        without(source, QUERY_LINE),                    # no start timestamp
        without(source, end_query),                     # no end timestamp
        without(source, "    s_rt_barriers++;\n"),
        without(source, "    phase_draw_batch_end();\n"),
        without(source, "    phase_draw_shutdown();\n"),
        without(source, "                    row_flags |= PHASE_DRAW_TARGETS;\n"),
        without(source, "                        row_flags |= PHASE_DRAW_VIEWPORT;\n"),
        without(source, "row_flags |= PHASE_DRAW_PSO; "),
        without(source, "row_flags |= PHASE_DRAW_SRV; "),
        without(source, "fclose(s_draw_csv); "),
        without(source, SWITCH),                        # a record that needs no switch
        without(source, "strrchr(phases, '\\\\')"),     # a backslash path loses its directory
    ]
    for index, mutant in enumerate(mutants):
        try:
            contract(mutant)
        except (AssertionError, ValueError):
            pass
        else:
            raise AssertionError("mutant %d accepted" % index)

    compiler = os.environ.get("CC") or shutil.which("clang-cl") or shutil.which("clang") or shutil.which("gcc")
    assert compiler, "C compiler required"
    environment = os.environ.copy()
    template = (Path(__file__).parent / "rsx_draw_record.c.in").read_text()
    code = template.replace("@BLOCK@", record_block(source)).replace("@LIMIT@", str(LIMIT))
    with tempfile.TemporaryDirectory(prefix="rsx-draw-record-") as temporary:
        path = Path(temporary)
        csv = path / "rsx_draws.csv"
        phases = str(csv.with_name("rsx_phases.csv"))
        exe = build(compiler, environment, path, "record", code)
        base = dict(environment)
        base.pop("RSX_PHASE_GPU_DRAWS", None)
        base.pop("RSX_PHASE_CSV", None)
        result = run(exe, base, "off", "off")
        assert "[RSXPHASE]" not in result.stderr and not csv.exists(), "the default run logs and writes no draw CSV"
        on = dict(base, RSX_PHASE_GPU_DRAWS="1", RSX_PHASE_CSV=phases)
        result = run(exe, dict(base, RSX_PHASE_CSV=phases), "noswitch", "noswitch")
        assert "[RSXPHASE]" not in result.stderr and not csv.exists(), "a path alone logs and writes no draw CSV"
        result = run(exe, dict(base, RSX_PHASE_GPU_DRAWS="1"), "nopath", "nopath")
        assert result.stderr.count("[RSXPHASE] Draw CSV off: RSX_PHASE_CSV gives no path\n") == 1, result.stderr
        assert not csv.exists(), "no file without a path"
        result = run(exe, on, "noclock", "noclock")
        assert result.stderr.count("[RSXPHASE] GPU timestamps unavailable; the draw CSV stays off\n") == 1, result.stderr
        assert not csv.exists(), "no file without a GPU timestamp frequency"
        result = run(exe, on, "on", "on")
        assert result.stderr.count("[RSXPHASE] Draw CSV %s (stops at %d rows)\n" % (csv, LIMIT)) == 1, result.stderr
        assert result.stderr.count("[RSXPHASE] Draw CSV stopped at %d rows: %s\n" % (LIMIT, csv)) == 1, \
            "one stop line: " + result.stderr
        lines = csv.read_text().splitlines()
        assert lines == EXPECTED and len(lines) == LIMIT + 1, lines
        # The record keeps the directory of RSX_PHASE_CSV, with either separator.
        csv.unlink()
        slashed = dict(environment, RSX_PHASE_GPU_DRAWS="1", RSX_PHASE_CSV=phases.replace("\\", "/"))
        result = subprocess.run([str(exe), "on"], env=slashed, capture_output=True, text=True, errors="replace",
                                timeout=30)
        assert result.returncode == 0 and "FAIL" not in result.stderr, result.stderr
        assert "[RSXPHASE] Draw CSV %s (stops at %d rows)\n" % (str(csv).replace("\\", "/"), LIMIT) in result.stderr, \
            result.stderr
        assert csv.read_text().splitlines() == EXPECTED, "a path with forward slashes writes the same file"
        # Fixture controls: the compiled block must fail on each of these.
        controls = [
            (reset_first(code), "reset before the write"),
            (without(code, "fclose(s_draw_csv); "), "file stays open"),
            (without(code, "dr->vs_idx >= 0 && dr->vs_idx < s_d3d.vp_vs_n ? s_d3d.vp_vs[dr->vs_idx].hash : 0", "0"),
             "vertex program hash lost"),
        ]
        for index, (changed, label) in enumerate(controls):
            csv.unlink(missing_ok=True)
            control = build(compiler, environment, path, "control%d" % index, changed)
            result = subprocess.run([str(control), "on"], env=on, capture_output=True, text=True, errors="replace",
                                    timeout=30)
            written = csv.read_text().splitlines() if csv.exists() else None
            assert result.returncode != 0 or "FAIL" in result.stderr or written != EXPECTED, \
                "control accepted: " + label
    print("PASS actual draw record block: default off, switch and path, no timestamp frequency, rows of a batch at "
          "the next batch, flags, barriers, gap and draw time, failed readback, row limit, shutdown, call sites, "
          "%d mutants, %d fixture controls" % (len(mutants), len(controls)))


if __name__ == "__main__":
    main()
