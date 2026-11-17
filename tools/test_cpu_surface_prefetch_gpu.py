"""Run actual backend coherent prefetch against an authored headless D3D12 producer."""

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adapter", choices=["warp", "hardware"], default="warp")
    args = parser.parse_args()
    assert os.name == "nt", "Actual D3D12 validation requires Windows"
    tasks = subprocess.check_output(
        ["tasklist", "/FI", "IMAGENAME eq ms1_recomp.exe", "/FO", "CSV", "/NH"],
        text=True,
    )
    assert not any(
        row and row[0].lower() == "ms1_recomp.exe"
        for row in csv.reader(tasks.splitlines())
    ), "Wait for the owned game to finish"
    compiler = os.environ.get("MV_CLANG") or shutil.which("clang-cl")
    assert compiler, "Prepare the VS compiler environment"
    video = ROOT / "libs/video"
    backend = (video / "rsx_d3d12_backend.c").read_text(encoding="utf-8")
    surface = (video / "rsx_cpu_surface.inc").read_text(encoding="utf-8")
    header, count = re.subn(
        r"(?m)^(int|void) (rsx_d3d12_backend_\w+\([^;]*;)",
        r"static \1 \2",
        (video / "rsx_d3d12_backend.h").read_text(encoding="utf-8"),
    )
    assert count == 4, "Review changed backend entry-point linkage"
    fixture = video / "tests/rsx_cpu_surface_prefetch_gpu.c"
    paths = [
        video / "rsx_d3d12_backend.c",
        video / "rsx_d3d12_backend.h",
        video / "rsx_cpu_surface.inc",
        video / "rsx_surface_readback.h",
        video / "rsx_cpu_surface_profile.h",
        fixture,
        Path(__file__),
    ]
    out = ROOT / "temp/checks" / f"surface-prefetch-gpu-{time.time_ns()}"
    out.mkdir(parents=True)
    report = {
        "qualification": (
            "Actual backend bodies, device, render-target clears, command lists, "
            "queue/fences, readback and packing. Authored data and blocking gates. "
            "Four unrelated entry points have static declarations for fixture linkage. "
            "No native-title, throughput or FPS acceptance."
        ),
        "adapter": args.adapter,
        "source_sha256": {
            str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in paths
        },
        "commands": [],
        "mutants_rejected": [],
    }
    env = {
        key: value for key, value in os.environ.items() if not key.startswith("RSX_")
    }
    env.update(RSX_TEST_ADAPTER=args.adapter, AUDIO_ENABLE="0")

    def command(cmd, label, enabled=True):
        selected = dict(env)
        if enabled:
            selected["RSX_CPU_SURFACE_PREFETCH"] = "1F00"
        result = subprocess.run(
            cmd, cwd=ROOT, env=selected, text=True, capture_output=True, timeout=90
        )
        (out / f"{label}.log").write_text(
            result.stdout + result.stderr, encoding="utf-8"
        )
        report["commands"].append(
            {
                "label": label,
                "command": cmd,
                "prefetch": enabled,
                "returncode": result.returncode,
            }
        )
        (out / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        return result

    def build(text, label):
        folder = out / label
        folder.mkdir()
        (folder / "backend-static.c").write_text(backend, encoding="utf-8")
        (folder / "rsx_cpu_surface.inc").write_text(text, encoding="utf-8")
        (folder / "rsx_d3d12_backend.h").write_text(header, encoding="utf-8")
        shutil.copyfile(fixture, folder / "gpu.c")
        exe = folder / "gpu.exe"
        cmd = [
            compiler,
            "/nologo",
            "/TC",
            "/std:c17",
            "/O1",
            "/MDd",
            "/UNDEBUG",
            "/Gy",
            "/Gw",
            "/clang:-fno-strict-aliasing",
            "/clang:-ffp-contract=off",
            "/I" + str(ROOT / "include"),
            "/I" + str(ROOT / "runtime/platform/win32"),
            "/I" + str(video),
            str(folder / "gpu.c"),
            str(video / "rsx_texture_layout.c"),
            "/Fo" + str(folder) + "/",
            "/Fe" + str(exe),
            "/link",
            "/OPT:REF",
            "/INCREMENTAL:NO",
            "dxguid.lib",
            "d3d12.lib",
            "dxgi.lib",
            "d3dcompiler.lib",
        ]
        result = command(cmd, label + "-build")
        assert result.returncode == 0, f"Build failed, see {out}/{label}-build.log"
        report.setdefault("binary_sha256", {})[label] = hashlib.sha256(
            exe.read_bytes()
        ).hexdigest()
        return exe

    exe = build(surface, "production")
    for enabled in (False, True):
        result = command([str(exe)], "production-" + str(int(enabled)), enabled)
        assert result.returncode == 0, f"GPU test failed, see {out}/result.json"
        assert f"PASS actual GPU coherent prefetch={int(enabled)}" in result.stdout
        print(result.stdout.strip())
    mutants = [
        (
            "wrong-generation",
            "!e->generation || e->generation!=r->gpu_generation || ",
            "!e->generation || ",
        ),
        (
            "wrong-layout",
            "r->cpu_address!=e->address || r->cpu_pitch!=e->pitch || ",
            "r->cpu_address!=e->address || ",
        ),
        (
            "skipped-copy-fence",
            '        e->waited++;cpu_surface_fence_block(s_cpu_surface_prefetch_fence,s_cpu_surface_prefetch_event,e->fence,"prefetch GPU fence wait");\n',
            "        e->waited++;\n",
        ),
        (
            "skipped-retirement-fence",
            '    if(e->generation)cpu_surface_fence_block(s_cpu_surface_prefetch_fence,s_cpu_surface_prefetch_event,e->fence,"prefetch GPU fence wait");\n',
            "",
        ),
    ]
    for label, before, after in mutants:
        assert surface.count(before) == 1, label
        exe = build(surface.replace(before, after), label)
        result = command([str(exe)], label + "-run")
        assert result.returncode == 7 and "GPU contract failed" in result.stderr, label
        report["mutants_rejected"].append(label)
        print("PASS actual GPU mutant", label)
    report["passed"] = True
    (out / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(out)


if __name__ == "__main__":
    main()
