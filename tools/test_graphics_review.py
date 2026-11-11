"""Compile actual alias ownership and texture typing against authored controls."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT


def function(source, name):
    match = re.search(r"static [^\n]+\b" + name + r"\([^;]*?\)\s*\{", source)
    assert match, name
    end = match.end()
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start() : end]


def main():
    if os.name == "nt" and (ROOT / "scripts/project.py").exists():
        from project import build_environment

        os.environ.update(build_environment())
    source = (RUNTIME / "libs/video/rsx_d3d12_native_depth.inc").read_text()
    backend = (RUNTIME / "libs/video/rsx_d3d12_backend.c").read_text()
    include_volume = "--depth-only" not in sys.argv
    parts = [
        function(source, "depth_alias_prepare"),
        function(source, "depth_alias_note_writes"),
    ]
    if include_volume:
        parts.extend(
            function(backend, name)
            for name in ["tex3d_on", "tex_unit_volume", "dr_cube_mask"]
        )
    production = "\n".join(parts)
    fixture = (ROOT / "tools/graphics_review_fixture.c").read_text()
    with tempfile.TemporaryDirectory(prefix="graphics-review-") as output:
        directory = Path(output)
        c = directory / "review.c"
        c.write_text(fixture.replace("/* PRODUCTION */", production))
        exe = directory / ("review.exe" if os.name == "nt" else "review")
        if os.name == "nt":
            compiler = os.environ.get("MV_CLANG") or shutil.which("clang-cl")
            command = [
                compiler,
                "/nologo",
                "/std:c17",
                "/O2",
                "/D_CRT_SECURE_NO_WARNINGS",
                str(c),
                "/Fe" + str(exe),
                "/Fo" + str(directory) + os.sep,
            ]
        else:
            compiler = os.environ.get("CC") or shutil.which("cc")
            command = [compiler, "-std=c17", "-O2", str(c), "-o", str(exe)]
        if include_volume:
            command.insert(1, "/DCHECK_VOLUME" if os.name == "nt" else "-DCHECK_VOLUME")
        subprocess.run(command, check=True, timeout=120)
        failed = []
        for mode in ["depth", "volume"] if include_volume else ["depth"]:
            for cube, volume in [("0", "0"), ("0", "1"), ("1", "0"), ("1", "1")]:
                result = subprocess.run(
                    [str(exe), mode, cube, volume],
                    env=dict(os.environ, RSX_CUBE_TEX=cube, RSX_TEX3D=volume),
                    timeout=15,
                )
                if result.returncode:
                    failed.append((mode, cube, volume))
        assert not failed, failed
    print(
        "PASS actual alias clear/MRT/write order and four independent texture-flag combinations"
    )


if __name__ == "__main__":
    main()
