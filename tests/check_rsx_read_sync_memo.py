"""The read sync memo against the direct mark, with the real runtime mark function.
Rejects mutants of the memo.

Usage (from any directory):
  python tests/check_rsx_read_sync_memo.py [--out DIR] [--src LIBS_VIDEO_DIR]
The compiler is MV_CLANG, CC, or clang-cl on Windows (cc on other systems).
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
KEY = "slot->address == address && slot->bytes == bytes && slot->bytes"

# name, text before, text after. Each mutant keeps the code compilable.
MUTANTS = [
    ("a slot holds any size at its address", KEY, "slot->address == address && slot->bytes"),
    ("a slot holds any address with its size", KEY, "slot->bytes == bytes && slot->bytes"),
    ("the memo is never used", "if (slot->address == address", "if (0 && slot->address == address"),
    ("no rule for the page before the range",
     "(!first || rsx_read_sync_memo_inside(memo, first - 1u))", "1u"),
    ("the marked pages are not recorded",
     "if (!(*byte & bit)) *byte = (unsigned char)(*byte | bit);", "(void)byte; (void)bit;"),
    ("one slot for each 16 MB of pages", "(page * 2654435761u) >> 12u", "page >> 12u"),
    ("a hit empties the slot", "        return 0;\n    const int result",
     "        { slot->bytes = 0; return 0; }\n    const int result"),
]

VARIANTS = {
    "production": [],
    "small": ["-DRSX_READ_SYNC_MEMO_ENTRIES=8u", "-DRSX_READ_SYNC_MEMO_PAGES=131072u"],
}


def replaced(text, before, after):
    assert text.count(before) == 1, before
    return text.replace(before, after)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=ROOT / "out" / "read-sync-memo")
    parser.add_argument("--src", type=Path, default=ROOT / "libs/video")
    arguments = parser.parse_args()
    out = arguments.out
    out.mkdir(parents=True, exist_ok=True)
    surface = (arguments.src / "rsx_cpu_surface.inc").read_text(encoding="utf-8")
    assert surface.count('#include "rsx_read_sync_memo.h"') == 1, "the include of the memo"
    assert surface.count("rsx_read_sync_memo_mark(&s_cpu_surface_mark_memo,") == 1, "the call of the memo"
    assert "vm_read_sync_mark_range(" not in surface, "the surface code marks without the memo"
    header = (arguments.src / "rsx_read_sync_memo.h").read_text(encoding="utf-8")
    fixture = ROOT / "tests/fixtures/rsx_read_sync_memo.c"
    mark_source = ROOT / "runtime/memory/guest_read_sync.c"
    compiler = os.environ.get("MV_CLANG") or os.environ.get("CC") or ("clang-cl" if os.name == "nt" else "cc")
    msvc = Path(compiler).name.lower() in {"cl", "cl.exe", "clang-cl", "clang-cl.exe"}

    def check(name, text, variant):
        """Builds the memo `text` with the fixture and runs both paths. Returns the new path run."""
        folder = out / (name + "_" + variant)
        folder.mkdir(exist_ok=True)
        (folder / "rsx_read_sync_memo.h").write_text(text, encoding="utf-8")
        exe = folder / ("check.exe" if os.name == "nt" else "check")
        include = [folder, arguments.src, ROOT / "include"]
        if msvc:
            flags = ["/nologo", "/O2", "/UNDEBUG", "/std:c17", *("/I" + str(p) for p in include),
                     "/Fo" + str(folder) + os.sep, "/Fe" + str(exe)]
        else:
            flags = ["-std=c11", "-O2", "-UNDEBUG", *("-I" + str(p) for p in include), "-o", str(exe)]
        cmd = [compiler, *flags, *VARIANTS[variant], str(fixture), str(mark_source)]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        (folder / "build.log").write_text(result.stdout + result.stderr)
        assert result.returncode == 0, name + " does not build:\n" + result.stdout + result.stderr
        dump = folder / "old.bin"
        old = subprocess.run([str(exe), "old", str(dump)], capture_output=True, text=True, timeout=60)
        new = subprocess.run([str(exe), "new", str(dump)], capture_output=True, text=True, timeout=60)
        (folder / "run.log").write_text(old.stdout + old.stderr + new.stdout + new.stderr)
        assert old.returncode == 0, name + ": the direct mark path failed:\n" + old.stdout + old.stderr
        return new

    for variant in VARIANTS:
        result = check("actual", header, variant)
        print(result.stdout + result.stderr, end="")
        if result.returncode:
            return 1
    rejected = []
    for name, before, after in MUTANTS:
        result = check("mutant_" + str(len(rejected) + 1), replaced(header, before, after), "production")
        lines = [line for line in (result.stdout + result.stderr).splitlines() if line.startswith("FAIL")]
        if result.returncode == 0:
            print("FAIL mutant survived: " + name)
            return 1
        rejected.append(name)
        print("PASS mutant rejected: " + name + (" -- " + lines[0][5:] if lines else ""))
    print(f"PASS: read sync memo ({len(rejected)} mutants rejected)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
