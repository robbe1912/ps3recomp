#!/usr/bin/env python3
"""Compile real ld emissions against saved TOCs, including a known primary TOC."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from ppu_disasm import decode
from ppu_lifter import LiftedFunction, PPULifter


def main():
    statements = []
    count = 0
    for primary in (0, 0x12345000):
        lifter = PPULifter()
        # Reproduce the former main()/worker known-TOC setup on the old code.
        lifter.toc_base = primary
        dummy = LiftedFunction(0x20000, "toc_load")
        for rd, base in ((2, 1), (2, 13), (3, 1)):
            for disp in (-16, 0, 24, 40):
                word = (58 << 26) | (rd << 21) | (base << 16) | (disp & 0xFFFC)
                insn = decode(word, 0x20000)
                assert insn.mnemonic == "ld", insn
                code = lifter._translate(insn, dummy)
                for saved in (0, primary, 0xFEDCBA9876543210):
                    count += 1
                    statements.append(f"""
    {{ Ctx state = {{}}; Ctx* ctx = &state;
       ctx->gpr[{base}] = 0x100; ctx->gpr[{rd}] = 0xBAD;
       expected_ea = 0x100 + ({disp}); calls = 0;
       for (int i = 0; i < 8; ++i)
           memory[expected_ea+i] = (uint8_t)(UINT64_C({saved}) >> (56-8*i));
       {code}
       if (ctx->gpr[{rd}] != UINT64_C({saved}) || calls != 1) {{
           printf("FAIL primary=%llx rd={rd} base={base} disp={disp} saved=%llx got=%llx reads=%d\\n",
                  (unsigned long long)UINT64_C({primary}), (unsigned long long)UINT64_C({saved}),
                  (unsigned long long)ctx->gpr[{rd}], calls); ++fail;
       }}
    }}""")
    source = """#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
struct Ctx { uint64_t gpr[32]; };
static uint8_t memory[512];
static unsigned expected_ea;
static int calls;
static uint64_t vm_read64(uint64_t ea) {
    if (ea != expected_ea || ea+8 > sizeof(memory)) abort();
    ++calls; uint64_t value = 0;
    for (int i=0; i<8; ++i) value = (value<<8) | memory[ea+i];
    return value;
}
int main() { int fail = 0;
""" + "\n".join(statements) + f"""
    printf("TOC stack loads: {count} cases, %d failed\\n", fail);
    return fail ? 1 : 0;
}}
"""
    compiler = os.environ.get("CXX") or shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise SystemExit("C++ compiler required; no emission-only success")
    with tempfile.TemporaryDirectory(prefix="ppu-toc-load-") as tmp:
        src = Path(tmp) / "toc.cpp"
        exe = Path(tmp) / ("toc.exe" if os.name == "nt" else "toc")
        src.write_text(source, encoding="utf-8")
        subprocess.run([compiler, "-std=c++11", "-O1", str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
