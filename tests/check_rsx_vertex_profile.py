#!/usr/bin/env python3
"""Check the production opt-in vertex counters with authored guest bytes."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import shutil

ROOT = Path(__file__).resolve().parents[1]
VIDEO = ROOT / "libs/video"
BACKEND = VIDEO / "rsx_d3d12_backend.c"
FETCH = VIDEO / "rsx_vertex_fetch.c"
TEMPLATE = ROOT / "tests/fixtures/rsx_vertex_profile.c.in"
OUT = ROOT / "build/vertex-profile-check"


def profile_block(text):
    start = text.index("/* RSX_VERTEX_PROFILE=1:")
    reader = text.index("static void read_vp_vertex", start)
    return text[start:text.index("\n}\n", reader) + 3]


def uploader(text):
    start = text.index("static u32 upload_strip_vp_indexed(")
    return text[start:text.index("\n}\n", start) + 3]


def compile_fixture(compiler, source, environment, name):
    folder = OUT / name
    folder.mkdir(parents=True, exist_ok=True)
    binary = folder / ("check.exe" if os.name == "nt" else "check")
    include = ROOT / "include"
    if Path(compiler).name.lower() in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"}:
        command = [compiler, "/nologo", "/TC", "/std:c17", "/O2", "/UNDEBUG",
                   "/I" + str(VIDEO), "/I" + str(include), str(source), str(FETCH),
                   "/Fo" + str(folder) + os.sep, "/Fe" + str(binary)]
    else:
        command = [compiler, "-std=c11", "-O2", "-UNDEBUG", "-I" + str(VIDEO),
                   "-I" + str(include), str(source), str(FETCH), "-o", str(binary)]
    result = subprocess.run(command, cwd=folder, env=environment,
                            capture_output=True, text=True, timeout=120)
    (folder / "build.log").write_text(result.stdout + result.stderr, encoding="utf-8")
    assert result.returncode == 0, result.stdout + result.stderr
    return binary, command


def run(binary, environment, name, scenario="single", enabled=1):
    env = dict(environment)
    for key in list(env):
        if key.startswith(("RSX_", "VP_", "VTX_", "PERF")):
            env.pop(key)
    env["RSX_VERTEX_PROFILE"] = str(enabled)
    result = subprocess.run([str(binary), scenario], cwd=binary.parent, env=env,
                            capture_output=True, text=True, timeout=30)
    (binary.parent / (name + ".run.log")).write_text(
        result.stdout + result.stderr, encoding="utf-8")
    assert result.returncode == 0, result.stdout + result.stderr
    lines = [line for line in result.stderr.splitlines() if line.startswith("[VTXPROFILE]")]
    clocks = [line for line in result.stdout.splitlines() if line.startswith("CLOCK ")]
    assert len(clocks) == 1, result.stdout + result.stderr
    return lines, int(clocks[0].split()[1]), result.stdout + result.stderr


def values(line):
    fields = dict((part.split("=")[0], part.split("=")[1])
                  for part in line.split() if "=" in part)
    return fields


def expect(actual, expected):
    lines, clock, output = actual
    assert len(lines) == 1, output
    fields = values(lines[0])
    for key, value in expected.items():
        if key == "clock":
            continue
        assert fields.get(key) == value, (key, fields.get(key), value, output)
    assert clock == expected["clock"], (clock, expected, output)


def main():
    environment = os.environ.copy()
    compiler = environment.get("CC") or shutil.which("clang-cl" if os.name == "nt" else "cc")
    assert compiler, "Set CC to a C11 compiler"
    text = BACKEND.read_text(encoding="utf-8")
    fixture = TEMPLATE.read_text(encoding="utf-8")
    assert fixture.count("@VERTEX_PROFILE_BLOCK@") == 1
    assert fixture.count("@INDEXED_STRIP_FUNCTION@") == 1
    source = OUT / "profile.c"
    OUT.mkdir(parents=True, exist_ok=True)
    source.write_text(fixture.replace("@VERTEX_PROFILE_BLOCK@", profile_block(text))
                      .replace("@INDEXED_STRIP_FUNCTION@", uploader(text)), encoding="utf-8")
    binary, command = compile_fixture(compiler, source, environment, "actual")
    records = dict(game_inputs=False, production_functions=["read_vp_vertex", "upload_strip_vp_indexed"],
                   source_sha256={str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                                  for path in (BACKEND, FETCH, TEMPLATE, Path(__file__))},
                   compiler=compiler, command=command)
    try:
        off = run(binary, environment, "off", enabled=0)
        reset = run(binary, environment, "reset", scenario="reset")
        assert len(reset[0]) == 2, reset[2]
        empty = dict(fetches="0", fetch_bytes="0", fetch_ms="0.000", draws="0",
                     repeats="0", hits="0", misses="0", direct_bytes="0",
                     copy_bytes="0", copy_ms="0.000")
        for key, value in empty.items():
            assert values(reset[0][1]).get(key) == value, (key, values(reset[0][1]), reset[2])
        assert not off[0] and off[1] == 0, off[2]
        # Each fetch or copy reads the clock at start and end. The fake clock
        # advances one microsecond per read, so each operation costs one microsecond.
        single = dict(fetches="4", fetch_bytes="1024", fetch_ms="0.004", draws="1",
                      repeats="0", hits="2", misses="4", direct_bytes="0",
                      copy_bytes="1536", copy_ms="0.006", clock=20)
        repeat = dict(fetches="12", fetch_bytes="3072", fetch_ms="0.012", draws="3",
                      repeats="2", hits="6", misses="12", direct_bytes="0",
                      copy_bytes="4608", copy_ms="0.018", clock=60)
        direct = dict(fetches="3", fetch_bytes="768", fetch_ms="0.003", draws="1",
                      repeats="0", hits="0", misses="0", direct_bytes="768",
                      copy_bytes="0", copy_ms="0.000", clock=6)
        expect(run(binary, environment, "single"), single)
        expect(run(binary, environment, "repeat", scenario="repeat"), repeat)
        expect(run(binary, environment, "direct", scenario="direct"), direct)
        mutants = {
            "fetch count": ("++s_vertex_profile.fetches;", ";"),
            "fetch bytes": ("sizeof(out16[0]) * 16", "sizeof(out16[0]) * 15"),
            "fetch time": ("s_vertex_profile.fetch_seconds += perf_now() - started;", ";"),
            "cache hit": ("++s_vertex_profile.cache_hits;", ";"),
            "cache miss": ("++s_vertex_profile.cache_misses;", ";"),
            "copy bytes": ("s_vertex_profile.copy_bytes += sizeof cached_vertices[set][way];", ";"),
            "copy time": ("s_vertex_profile.copy_seconds += perf_now() - started;", ";"),
            "draw count": ("++s_vertex_profile.draws;", ";"),
            "repeat count": ("++s_vertex_profile.repeats;", ";"),
        }
        for name, (before, after) in mutants.items():
            assert text.count(before) == 1, (name, text.count(before))
            mutant_text = text.replace(before, after, 1)
            mutant_source = OUT / ("mutant-" + name.replace(" ", "-") + ".c")
            mutant_source.write_text(fixture.replace("@VERTEX_PROFILE_BLOCK@", profile_block(mutant_text))
                                     .replace("@INDEXED_STRIP_FUNCTION@", uploader(mutant_text)), encoding="utf-8")
            mutant_binary, _ = compile_fixture(compiler, mutant_source, environment,
                                                "mutant-" + name.replace(" ", "-"))
            scenario, expected = ("repeat", repeat) if name == "repeat count" else ("single", single)
            try:
                expect(run(mutant_binary, environment, "mutant", scenario=scenario), expected)
            except (AssertionError, IndexError):
                print("PASS rejected mutant:", name)
            else:
                raise AssertionError("mutant passed: " + name)
        print("PASS: opt-in vertex counters, byte parity, repeat ranges, cache accounting, clocks and reset")
    finally:
        (OUT / "result.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
        print("Evidence:", OUT)


if __name__ == "__main__":
    main()
