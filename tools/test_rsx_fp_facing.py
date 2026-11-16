"""Check FACING HLSL on every host and actual WARP shader draws on Windows."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
VIDEO = ROOT / "libs/video"
OUT = ROOT / "build/fp-facing-tests"
CASE = '    case 0xE: return "(input.rsx_front ? float4(1,1,1,1) : float4(-1,-1,-1,-1))";\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler")
    args = parser.parse_args()
    compiler = args.compiler or os.environ.get("MV_CLANG") or os.environ.get("CC")
    compiler = compiler or ("clang-cl" if os.name == "nt" else "cc")
    compiler = shutil.which(compiler) or compiler
    msvc = Path(compiler).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe")
    OUT.mkdir(parents=True, exist_ok=True)
    source = (VIDEO / "rsx_fp_decompiler.c").read_text(encoding="utf-8")
    assert source.count(CASE) == 1
    baseline = OUT / "rsx_fp_decompiler.c"
    baseline.write_text(source.replace(CASE, ""), encoding="utf-8")
    commands = []

    def execute(command, label):
        result = subprocess.run(
            command, cwd=OUT, capture_output=True, text=True, timeout=120
        )
        (OUT / (label + ".log")).write_text(
            result.stdout + result.stderr, encoding="utf-8"
        )
        commands.append(
            {"label": label, "command": command, "returncode": result.returncode}
        )
        return result

    objects = {}
    for label, path in (
        ("production", VIDEO / "rsx_fp_decompiler.c"),
        ("zero", baseline),
    ):
        obj = OUT / (label + (".obj" if msvc else ".o"))
        if msvc:
            command = [
                compiler,
                "/nologo",
                "/std:c17",
                "/O2",
                "/UNDEBUG",
                "/D_CRT_SECURE_NO_WARNINGS",
                "/I" + str(VIDEO),
                "/I" + str(ROOT / "include"),
                "/c",
                str(path),
                "/Fo" + str(obj),
            ]
        else:
            command = [
                compiler,
                "-std=c17",
                "-O2",
                "-UNDEBUG",
                "-I" + str(VIDEO),
                "-I" + str(ROOT / "include"),
                "-c",
                str(path),
                "-o",
                str(obj),
            ]
        result = execute(command, label + "-compile")
        assert not result.returncode, result.stdout + result.stderr
        objects[label] = obj

    plain = []
    fixture = VIDEO / "tests/test_fp_facing.c"
    for label, obj in objects.items():
        exe = OUT / (label + (".exe" if os.name == "nt" else ""))
        if msvc:
            command = [
                compiler,
                "/nologo",
                "/O2",
                "/UNDEBUG",
                "/D_CRT_SECURE_NO_WARNINGS",
                "/I" + str(VIDEO),
                "/I" + str(ROOT / "include"),
                str(fixture),
                str(obj),
                "/Fe" + str(exe),
            ]
        else:
            command = [
                compiler,
                "-std=c17",
                "-O2",
                "-UNDEBUG",
                "-I" + str(VIDEO),
                "-I" + str(ROOT / "include"),
                str(fixture),
                str(obj),
                "-lm",
                "-o",
                str(exe),
            ]
        result = execute(command, label + "-link")
        assert not result.returncode, result.stderr
        result = execute([str(exe), "--plain"], label + "-plain")
        assert not result.returncode
        plain.append(result.stdout)
        result = execute([str(exe)], label + "-run")
        assert (result.returncode == 0) == (label == "production"), (
            result.stdout + result.stderr
        )
    assert plain[0] == plain[1], "non-FACING program changed"

    if os.name == "nt":
        running = subprocess.check_output(
            ["tasklist", "/FI", "IMAGENAME eq ms1_recomp.exe", "/FO", "CSV"], text=True
        )
        assert '"ms1_recomp.exe"' not in running.lower(), "native title owns GPU host"
        for label, obj in objects.items():
            exe = OUT / (label + "-gpu.exe")
            command = [
                compiler,
                "/nologo",
                "/std:c++17",
                "/EHsc",
                "/O2",
                "/UNDEBUG",
                "/D_CRT_SECURE_NO_WARNINGS",
                "/I" + str(VIDEO),
                "/I" + str(ROOT / "include"),
                str(VIDEO / "tests/rsx_fp_facing_gpu.cpp"),
                str(obj),
                "/Fe" + str(exe),
                "/link",
                "d3d11.lib",
                "dxgi.lib",
                "d3dcompiler.lib",
            ]
            result = execute(command, label + "-gpu-build")
            assert not result.returncode, result.stdout + result.stderr
            result = execute([str(exe)], label + "-gpu-run")
            if label == "production":
                assert (
                    result.returncode == 0 and "checks=4 failures=0" in result.stdout
                ), result.stdout + result.stderr
            else:
                assert result.returncode == 1 and "FAIL" in result.stdout, (
                    result.stdout + result.stderr
                )
    report = {
        "commands": commands,
        "decompiler_sha256": hashlib.sha256(source.encode()).hexdigest(),
        "non_facing_byte_equal": True,
        "gpu_warp": os.name == "nt",
    }
    (OUT / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(
        "PASS FACING source and zero-input control, unchanged plain HLSL"
        + (
            "; WARP four checks"
            if os.name == "nt"
            else "; GPU coverage runs on Windows"
        )
    )


if __name__ == "__main__":
    main()
