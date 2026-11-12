#!/usr/bin/env python3
"""Compile actual VMX FMA emissions against an exact binary32 oracle."""

from fractions import Fraction
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT
sys.path.insert(0, str(RUNTIME / "tools"))
from ppu_disasm import decode
from ppu_lifter import PPULifter


def finite(bits):
    exponent = (bits >> 23) & 255
    mantissa = bits & 0x7FFFFF
    power = exponent - 150 if exponent else -149
    magnitude = Fraction(mantissa | (0x800000 if exponent else 0))
    magnitude *= Fraction(2) ** power
    return -magnitude if bits >> 31 else magnitude


def rounded(value, negative_zero=False):
    if not value:
        return 0x80000000 if negative_zero else 0
    sign = 0x80000000 if value < 0 else 0
    value = abs(value)
    exponent = value.numerator.bit_length() - value.denominator.bit_length()
    if value < Fraction(2) ** exponent:
        exponent -= 1
    scale = 149 if exponent < -126 else 23 - exponent
    scaled = value * Fraction(2) ** scale
    whole, remainder = divmod(scaled.numerator, scaled.denominator)
    if 2 * remainder > scaled.denominator or (
        2 * remainder == scaled.denominator and whole & 1
    ):
        whole += 1
    if exponent < -126:
        return sign | whole
    if whole == 1 << 24:
        whole >>= 1
        exponent += 1
    if exponent > 127:
        return sign | 0x7F800000
    return sign | ((exponent + 127) << 23) | (whole & 0x7FFFFF)


def flush(bits, nj):
    return bits & 0x80000000 if nj and bits & 0x7F800000 == 0 else bits


def fused(a, b, c):
    if any(x & 0x7FFFFFFF > 0x7F800000 for x in (a, b, c)):
        return 0x7FC00000
    product_sign = (a ^ c) & 0x80000000
    a_inf, c_inf = a & 0x7FFFFFFF == 0x7F800000, c & 0x7FFFFFFF == 0x7F800000
    if (a_inf and c & 0x7FFFFFFF == 0) or (c_inf and a & 0x7FFFFFFF == 0):
        return 0x7FC00000
    if a_inf or c_inf:
        if b & 0x7FFFFFFF == 0x7F800000 and (b ^ product_sign) >> 31:
            return 0x7FC00000
        return product_sign | 0x7F800000
    if b & 0x7FFFFFFF == 0x7F800000:
        return b
    negative_zero = product_sign == 0x80000000 and b == 0x80000000
    return rounded(finite(a) * finite(c) + finite(b), negative_zero)


def reference(op, a, b, c, nj):
    a, b, c = (flush(x, nj) for x in (a, b, c))
    result = fused(a, b ^ (0x80000000 if op else 0), c)
    if op and result & 0x7FFFFFFF <= 0x7F800000:
        result ^= 0x80000000
    return flush(result, nj)


def main():
    out = ROOT / "temp/checks" / f"ppu-vmx-fused-{time.time_ns()}"
    out.mkdir(parents=True)
    header = RUNTIME / "runtime/ppu/ppu_vmx.h"
    report = {"sources": {}, "commands": []}
    for path in (Path(__file__), RUNTIME / "tools/ppu_lifter.py", header):
        if path.exists():
            report["sources"][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()

    def run(command, **kwargs):
        result = subprocess.run(command, capture_output=True, timeout=120, **kwargs)
        report["commands"].append(
            {
                "command": command,
                "returncode": result.returncode,
                "stdout": result.stdout.decode(errors="replace"),
                "stderr": result.stderr.decode(errors="replace"),
            }
        )
        (out / "result.json").write_text(json.dumps(report, indent=2))
        assert result.returncode == 0, result.stderr.decode(errors="replace")
        return result.stdout

    functions = []
    names = []
    for op, mnemonic in enumerate(("vmaddfp", "vnmsubfp")):
        row = []
        for dest in range(1, 5):
            word = 4 << 26 | dest << 21 | 1 << 16 | 2 << 11 | 3 << 6 | (46 + op)
            instruction = decode(word, 0x1000)
            assert instruction.mnemonic == mnemonic
            statement = PPULifter()._translate_op(instruction, None)
            name = f"step_{op}_{dest}"
            row.append(name)
            functions.append(f"void {name}(Context* ctx) {{ {statement} }}")
        names.append("{" + ",".join(row) + "}")
    source = r"""
#include <cstdint>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif
struct Context { alignas(16) unsigned char vr[32][16]; uint32_t vscr; };
uint32_t getword(const void* p, unsigned i) {
 const auto* b = static_cast<const unsigned char*>(p) + 4*i;
 return uint32_t(b[0])<<24 | uint32_t(b[1])<<16 | uint32_t(b[2])<<8 | b[3];
}
void putword(void* p, unsigned i, uint32_t x) {
 auto* b = static_cast<unsigned char*>(p) + 4*i;
 b[0]=x>>24; b[1]=x>>16; b[2]=x>>8; b[3]=x;
}
void ppu_vldf4(const void* p, float* out) {
 for(unsigned i=0;i<4;++i) { uint32_t x=getword(p,i); std::memcpy(out+i,&x,4); }
}
void ppu_vstf4(void* p, const float* in) {
 for(unsigned i=0;i<4;++i) { uint32_t x; std::memcpy(&x,in+i,4); putword(p,i,x); }
}
"""
    if header.exists():
        source += '#include "ppu_vmx.h"\n'
    source += "\n".join(functions)
    source += (
        "\nusing Step=void(*)(Context*); Step steps[2][4]={" + ",".join(names) + "};\n"
    )
    source += r"""
int main() {
#ifdef _WIN32
 _setmode(_fileno(stdin), _O_BINARY);
 _setmode(_fileno(stdout), _O_BINARY);
#endif
 uint32_t words[15];
 while(std::fread(words,sizeof(words),1,stdin)==1) {
  Context context{}; context.vscr=words[2];
  for(unsigned reg=1;reg<=3;++reg) for(unsigned lane=0;lane<4;++lane)
   putword(context.vr[reg],lane,words[3+(reg-1)*4+lane]);
  steps[words[0]][words[1]-1](&context);
  uint32_t result[4];
  for(unsigned i=0;i<4;++i) result[i]=getword(context.vr[words[1]],i);
  if(std::fwrite(result,sizeof(result),1,stdout)!=1) return 2;
 }
 return std::ferror(stdin) ? 3 : 0;
}
"""
    path = out / "probe.cpp"
    path.write_text(source)
    report["generated_source_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    triples = [
        (0x3F800001, 0xBF800000, 0x3F7FFFFE),
        (0xBF800000, 0x3F800000, 0x3F800000),
        (0x7F7FFFFF, 0xFF7FFFFF, 0x40000000),
        (1, 0, 0x3F000000),
        (0x80000001, 0x80000000, 0x3F000000),
        (0x7F800000, 0, 0),
        (0x7FC12345, 0, 0x3F800000),
        (0x3F800000, 0x7F800000, 0x3F800000),
    ]
    triples += [
        (a, b, c)
        for a in (0, 0x80000000)
        for b in (0, 0x80000000)
        for c in (0, 0x80000000)
    ]
    rng = random.Random(302)
    triples += [tuple(rng.getrandbits(32) for _ in range(3)) for _ in range(128)]
    payload, expected = bytearray(), bytearray()
    for op in range(2):
        for dest in range(1, 5):
            for nj in (0, 0x10000):
                for index in range(len(triples)):
                    lanes = [
                        triples[(index + lane * 7) % len(triples)] for lane in range(4)
                    ]
                    words = [op, dest, nj] + [
                        v
                        for component in range(3)
                        for v in (lane[component] for lane in lanes)
                    ]
                    payload += struct.pack("<15I", *words)
                    expected += struct.pack(
                        "<4I", *(reference(op, *lane, nj) for lane in lanes)
                    )
    (out / "input.bin").write_bytes(payload)
    (out / "expected.bin").write_bytes(expected)
    compiler = (
        os.environ.get("MV_CLANG")
        or shutil.which("clang-cl" if os.name == "nt" else "clang++")
        or shutil.which("g++")
    )
    assert compiler, "C++ compiler required"
    executable = out / ("probe.exe" if os.name == "nt" else "probe")
    if os.name == "nt":
        command = [
            compiler,
            "/nologo",
            "/std:c++20",
            "/O2",
            "/EHsc",
            "/fp:strict",
            "-ffp-contract=off",
            str(path),
            f"/I{header.parent}",
            f"/Fo{out / 'probe.obj'}",
            f"/Fe{executable}",
        ]
    else:
        command = [
            compiler,
            "-std=c++20",
            "-O2",
            "-ffp-contract=off",
            str(path),
            f"-I{header.parent}",
            "-o",
            str(executable),
            "-lm",
        ]
    run(command)
    actual = run([str(executable)], input=bytes(payload))
    (out / "actual.bin").write_bytes(actual)
    assert len(actual) == len(expected)
    mismatches = sum(
        a != b
        for a, b in zip(
            struct.iter_unpack("<I", actual), struct.iter_unpack("<I", expected)
        )
    )
    report.update(vectors=len(payload) // 60, mismatches=mismatches)
    (out / "result.json").write_text(json.dumps(report, indent=2))
    assert mismatches == 0, f"Actual emitted VMX FMA differs in {mismatches} lanes"
    mutants = {
        "unfused": [
            ("fmaf(x, z, y)", "(x * z + y)"),
            ("fmaf(x, z, -y)", "(x * z - y)"),
        ],
        "wrong-addend": [("fmaf(x, z, y)", "fmaf(x, y, z)")],
        "missing-nj": [("#define VMX_NJ  0x00010000u", "#define VMX_NJ  0x00000000u")],
    }
    report["rejected_mutants"] = []
    for name, replacements in mutants.items():
        mutant_dir = out / name
        mutant_dir.mkdir()
        text = header.read_text()
        for before, after in replacements:
            assert before in text, (name, before)
            text = text.replace(before, after)
        (mutant_dir / header.name).write_text(text)
        mutant_command = list(command)
        mutant_command.insert(1, ("/I" if os.name == "nt" else "-I") + str(mutant_dir))
        run(mutant_command)
        mutant_output = run([str(executable)], input=bytes(payload))
        assert len(mutant_output) == len(expected) and mutant_output != expected, name
        report["rejected_mutants"].append(name)
        (out / "result.json").write_text(json.dumps(report, indent=2))
    print(
        f"VMX fused emission: {report['vectors']} vectors, exact binary32 oracle, four destination aliases, both NJ modes, three mutants: {out / 'result.json'}"
    )


if __name__ == "__main__":
    main()
