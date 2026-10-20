#!/usr/bin/env python3
"""Real ELF/CLI/compiled regression: executable sections beyond named functions."""
import json
import locale
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile


def elf_fixture(path):
    base = 0x1000
    main = 0x2000
    # Valid non-leaf caller. Its TOC restore depends on the .init linkage stub.
    words = [0x7C0802A6, 0xF8010010, 0xF821FF91]
    words += [0x48000001 | ((0x1000 - (main + 12)) & 0x3FFFFFC),
              0xE8410028]
    for target in (0x3000, 0x4000, 0x2800):
        words.append(0x48000001 | ((target - (main + 4*len(words))) & 0x3FFFFFC))
    words += [0xE8010080, 0x7C0803A6, 0x38210070, 0x4E800020]
    regions = [('.init', 0x1000, 6, [0xF8410028, 0x38420100, 0x3860000A, 0x4E800020]),
               ('.text', main, 6, words),
               ('.rodata', 0x2800, 2, [0x38800063, 0x4E800020]),
               ('.fini', 0x3000, 6, [0x38630014, 0x4E800020]),
               ('.sceStub.text', 0x4000, 6, [0x3863000C, 0x4E800020]),
               ('.opd', 0x4800, 2, [main, 0x12340000])]
    image = bytearray(0x4000)
    for _, va, _, data in regions:
        struct.pack_into('>' + 'I'*len(data), image, va-base, *data)
    names = b'\0'
    offsets = {}
    for name in [r[0] for r in regions] + ['.shstrtab']:
        offsets[name] = len(names)
        names += name.encode() + b'\0'
    data_offset = 0x100
    names_offset = data_offset + len(image)
    shoff = (names_offset + len(names) + 7) & ~7
    result = bytearray(shoff + 64*(len(regions)+2))
    ident = b'\x7fELF\x02\x02\x01' + bytes(9)
    struct.pack_into('>16sHHIQQQIHHHHHH', result, 0, ident, 2, 21, 1,
                     0x4800, 64, shoff, 0, 64, 56, 1, 64, len(regions)+2, len(regions)+1)
    struct.pack_into('>IIQQQQQQ', result, 64, 1, 5, data_offset, base, base,
                     len(image), len(image), 0x100)
    result[data_offset:data_offset+len(image)] = image
    result[names_offset:names_offset+len(names)] = names
    for i, (name, va, flags, data) in enumerate(regions, 1):
        struct.pack_into('>IIQQQQIIQQ', result, shoff+64*i, offsets[name], 1,
                         flags, va, data_offset+va-base, 4*len(data), 0, 0, 4, 0)
    struct.pack_into('>IIQQQQIIQQ', result, shoff+64*(len(regions)+1),
                     offsets['.shstrtab'], 3, 0, 0, names_offset, len(names), 0, 0, 1, 0)
    path.write_bytes(result)
    return [{'start': hex(main), 'end': hex(main+4*len(words))}]


def compile_run(out, compiler, expected):
    # Compile the exact CLI-emitted definitions with only the memory/dispatch
    # seam supplied here. No substitution of lifted function bodies.
    definitions = []
    for source in sorted(out.glob('*.cpp')):
        text = source.read_text(encoding=locale.getpreferredencoding(False))
        for match in re.finditer(r'void (func_[0-9A-F]+)\(ppu_context\* ctx\) \{', text):
            start, pos, depth = match.start(), match.end(), 1
            while depth:
                depth += (text[pos] == '{') - (text[pos] == '}')
                pos += 1
            definitions.append((match.group(1), text[start:pos]))
    prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
struct ppu_context { uint64_t gpr[32], lr; };
static unsigned char memory[4096];
static void (*g_trampoline_fn)(void*);
#define DRAIN_TRAMPOLINE(ctx) do { while(g_trampoline_fn) { auto f=g_trampoline_fn; g_trampoline_fn=nullptr; f(ctx); } } while(0)
static void vm_write64(uint64_t a,uint64_t v) { if(a+8>sizeof(memory)) abort(); for(int i=0;i<8;i++) memory[a+i]=(uint8_t)(v>>(56-8*i)); }
static uint64_t vm_read64(uint64_t a) { if(a+8>sizeof(memory)) abort(); uint64_t v=0;for(int i=0;i<8;i++) v=(v<<8)|memory[a+i];return v; }
'''
    driver = prelude + '\n'.join('void '+name+'(ppu_context*);' for name, _ in definitions)
    driver += '\n' + '\n'.join(body for _, body in definitions)
    driver += '''
int main() { ppu_context ctx={};ctx.gpr[1]=0x800;ctx.gpr[2]=0x12340000;
func_00002000(&ctx);DRAIN_TRAMPOLINE(&ctx);
printf("r3=%llu r2=%llx r4=%llu sp=%llx\\n",(unsigned long long)ctx.gpr[3],(unsigned long long)ctx.gpr[2],(unsigned long long)ctx.gpr[4],(unsigned long long)ctx.gpr[1]);
return !(ctx.gpr[3]==EXPECTED && ctx.gpr[2]==0x12340000 && ctx.gpr[4]==0 && ctx.gpr[1]==0x800); }
'''.replace('EXPECTED', str(expected))
    cpp, exe = out/'driver.cpp', out/('driver.exe' if os.name=='nt' else 'driver')
    cpp.write_text(driver, encoding='utf-8')
    subprocess.run([compiler, '-std=c++17', '-O1', str(cpp), '-o', str(exe)], check=True)
    return subprocess.run([str(exe)], check=False).returncode


def main():
    compiler = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
    if not compiler:
        raise RuntimeError('A C++ compiler is required; emission-only is insufficient')
    failed = 0
    with tempfile.TemporaryDirectory(prefix='ppu-exec-sections-') as td:
        root = Path(td)
        elf = root/'sections.elf'
        functions = root/'functions.json'
        functions.write_text(json.dumps(elf_fixture(elf)), encoding='utf-8')
        for jobs in (1, 2):
            for limit, expected in ((None, 42), ('0x3000', 10)):
                out = root/f'out-{jobs}-{limit}'
                cmd = [sys.executable, str(Path(__file__).with_name('ppu_lifter.py')),
                       str(elf), '--functions', str(functions), '--output', str(out),
                       '--source-name', 'ppu_recomp.cpp', '--jobs', str(jobs)]
                if limit:
                    cmd += ['--code-end', limit]
                subprocess.run(cmd, check=True, stdout=subprocess.PIPE, text=True)
                try:
                    rc = compile_run(out, compiler, expected)
                except subprocess.CalledProcessError:
                    rc = 1
                print(f'jobs={jobs} limit={limit} expected={expected}: '+('PASS' if rc==0 else 'FAIL'), flush=True)
                failed += rc != 0
    print(f'4 CLI compile/execution cases, {failed} failed')
    return int(failed != 0)


if __name__ == '__main__':
    raise SystemExit(main())
