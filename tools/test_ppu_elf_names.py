#!/usr/bin/env python3
"""ELF/CLI regression: function names from the ELF symbol table.

The C++ setjmp/longjmp lowering reads function names. A lift without ``--names``
lost every name, so no ``longjmp`` threw and no ``setjmp`` caller got a resume
point. The lifter now reads the function symbols of the ELF itself, and it stops
when a lifted setjmp or longjmp function got no lowering."""
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

LIFTER = Path(__file__).with_name('ppu_lifter.py')
SETJMP, LONGJMP, PLAIN, HIDDEN = 0x1100, 0x1180, 0x1200, 0x1280
MAIN_WORDS = [0x7C0802A6, 0xF821FF91,                 # mflr r0; stdu r1,-0x70(r1)
              0x48000001 | ((SETJMP - 0x1008) & 0x3FFFFFC),
              0x48000001 | ((PLAIN - 0x100C) & 0x3FFFFFC),
              0x48000001 | ((LONGJMP - 0x1010) & 0x3FFFFFC),
              0x38630001, 0x4E800020]                 # addi r3,r3,1; blr
CODE = [(0x1000, MAIN_WORDS), (SETJMP, [0x38600000, 0x4E800020]),
        (LONGJMP, [0x38600000, 0x4E800020]),
        (PLAIN, [0x48000001 | ((HIDDEN - 0x1200) & 0x3FFFFFC), 0x4E800020]),
        (HIDDEN, [0x38600007, 0x4E800020])]
# (name, address, type): STT_FUNC is 2. The alias shares the address of .plain.
SYMBOLS = [('.start', 0x1000, 2), ('.setjmp', SETJMP, 2), ('.longjmp', LONGJMP, 2),
           ('.plain', PLAIN, 2), ('.data_sym', PLAIN, 1), ('plain_alias', PLAIN, 2),
           ('hidden_data', HIDDEN, 1)]
# The caller does not call setjmp: the lift has a throw line and no resume point.
LONELY_CODE = [(0x1000, MAIN_WORDS[:2] + [0x60000000] + MAIN_WORDS[3:])] + CODE[1:]


def elf_fixture(path, symbols, code=CODE):
    """Write an ELF64 big-endian image with one R-X segment, a symbol table
    (unless ``symbols`` is empty) and the code above."""
    base = 0x1000
    image = bytearray(0x400)
    for address, words in code:
        struct.pack_into('>' + 'I' * len(words), image, address - base, *words)
    string_names = ['.text', '.symtab', '.strtab', '.shstrtab']
    offsets, names = {}, b'\0'
    for name in string_names:
        offsets[name] = len(names)
        names += name.encode() + b'\0'
    strtab = b'\0'
    symbol_name = {}
    for name, _, _ in symbols:
        symbol_name[name] = len(strtab)
        strtab += name.encode() + b'\0'
    entries = [bytes(24)] + [struct.pack('>IBBHQQ', symbol_name[name], (1 << 4) | kind,
                                        0, 1, address, 4) for name, address, kind in symbols]
    symtab = b''.join(entries)
    image_offset = 0x100
    strtab_offset = image_offset + len(image)
    symtab_offset = strtab_offset + len(strtab)
    names_offset = symtab_offset + len(symtab)
    shoff = (names_offset + len(names) + 7) & ~7
    count = 5 if symbols else 3            # null, .text, [.symtab, .strtab], .shstrtab
    result = bytearray(shoff + 64 * count)
    ident = b'\x7fELF\x02\x02\x01' + bytes(9)
    struct.pack_into('>16sHHIQQQIHHHHHH', result, 0, ident, 2, 21, 1, 0x4800, 64,
                     shoff, 0, 64, 56, 1, 64, count, count - 1)
    struct.pack_into('>IIQQQQQQ', result, 64, 1, 5, image_offset, base, base,
                     len(image), len(image), 0x100)
    result[image_offset:image_offset + len(image)] = image
    result[strtab_offset:strtab_offset + len(strtab)] = strtab
    result[symtab_offset:symtab_offset + len(symtab)] = symtab
    result[names_offset:names_offset + len(names)] = names
    rows = [(offsets['.text'], 1, 6, base, image_offset, len(image), 0, 0, 4, 0)]
    if symbols:
        rows += [(offsets['.symtab'], 2, 0, 0, symtab_offset, len(symtab), 3, 1, 8, 24),
                 (offsets['.strtab'], 3, 0, 0, strtab_offset, len(strtab), 0, 0, 1, 0)]
    rows += [(offsets['.shstrtab'], 3, 0, 0, names_offset, len(names), 0, 0, 1, 0)]
    for i, row in enumerate(rows, 1):
        struct.pack_into('>IIQQQQIIQQ', result, shoff + 64 * i, *row)
    path.write_bytes(result)
    return [{'start': hex(0x1000), 'end': hex(0x1000 + 4 * len(code[0][1]))}]


def lift(root, name, elf, functions, *, names=None, lowering=True, jobs=1):
    out = root / name
    cmd = [sys.executable, str(LIFTER), str(elf), '--functions', str(functions),
           '--output', str(out), '--source-name', 'ppu_recomp.cpp', '--jobs', str(jobs)]
    if names:
        cmd += ['--names', str(names)]
    if lowering:
        cmd += ['--cpp-nonlocal-jumps']
    run = subprocess.run(cmd, capture_output=True, text=True)
    text = ''.join(p.read_text(encoding='utf-8') for p in sorted(out.glob('*.cpp')))
    return run, text


def labels(text):
    """Function name -> label comment line above its definition ('' when none)."""
    found = {}
    lines = text.splitlines()
    for i, line in enumerate(lines):
        match = re.fullmatch(r'void (func_[0-9A-F]+)\(ppu_context\* ctx\) \{', line)
        if match:
            found[match[1]] = lines[i - 1].strip() if i and lines[i - 1].lstrip().startswith('/*') else ''
    return found


def body(text, name):
    start = text.find(f'void {name}(ppu_context* ctx) {{')
    return '' if start < 0 else text[start:text.find('\n}\n', start)]


def check(case, condition, detail=''):
    print(f'{case}: ' + ('PASS' if condition else 'FAIL ' + detail), flush=True)
    return not condition


def main():
    failed = 0
    with tempfile.TemporaryDirectory(prefix='ppu-elf-names-') as td:
        root = Path(td)
        elf, bare, functions = root/'names.elf', root/'bare.elf', root/'functions.json'
        functions.write_text(json.dumps(elf_fixture(elf, SYMBOLS)), encoding='utf-8')
        elf_fixture(bare, [])
        override, added = root/'override.json', root/'added.json'
        override.write_text(json.dumps({'0x00001100': {'label': 'not_setjmp'},
                                        '0x00001180': {'label': 'not_longjmp'}}), encoding='utf-8')
        added.write_text(json.dumps({'0x00001200': {'label': 'override'},
                                     '0x00001280': {'label': 'added'}}), encoding='utf-8')

        run, text = lift(root, 'default', elf, functions)
        found = labels(text)
        failed += check('default names: exit 0', run.returncode == 0, run.stderr[-400:])
        failed += check('default names: count line',
                        '  names: 4 function symbols from the ELF symbol table\n' in run.stdout,
                        run.stdout[-200:])
        for name, label in (('func_00001000', '/* .start */'), ('func_00001100', '/* .setjmp */'),
                            ('func_00001180', '/* .longjmp */'), ('func_00001200', '/* .plain */'),
                            ('func_00001280', '')):
            failed += check(f'default names: {name} has [{label}]', found.get(name) == label,
                            repr(found.get(name)))
        failed += check('default names: no data symbol and no alias name',
                        'data_sym' not in text and 'plain_alias' not in text)
        failed += check('default names: longjmp throws',
                        'throw ppu_nonlocal_jump{ctx, (uint32_t)ctx->lr, ctx->gpr[1]};'
                        in body(text, 'func_00001180'))
        main_body = body(text, 'func_00001000')
        failed += check('default names: setjmp caller has one resume point',
                        main_body.count('_nonlocal_sp_0000100C = ctx->gpr[1];') == 1
                        and 'loc_nonlocal_0000100C:' in main_body
                        and 'catch (const ppu_nonlocal_jump& jump)' in main_body, main_body)

        run2, text2 = lift(root, 'jobs2', elf, functions, jobs=2)
        failed += check('two jobs: same source and exit 0',
                        run2.returncode == 0 and text2 == text, run2.stderr[-400:])

        run, text = lift(root, 'added', elf, functions, names=added)
        found = labels(text)
        failed += check('--names adds and overrides: exit 0', run.returncode == 0, run.stderr[-400:])
        failed += check('--names adds and overrides: lines',
                        '  Loaded 2 recovered names from' in run.stdout
                        and '  names: 4 function symbols' in run.stdout)
        failed += check('--names adds and overrides: labels',
                        found.get('func_00001200') == '/* override */'
                        and found.get('func_00001280') == '/* added */'
                        and found.get('func_00001180') == '/* .longjmp */'
                        and '/* .plain */' not in text, repr(found))
        failed += check('--names adds and overrides: lowering kept',
                        'throw ppu_nonlocal_jump{' in body(text, 'func_00001180'))

        run, text = lift(root, 'override', elf, functions, names=override)
        failed += check('guard: exit 1 when the lowering is lost', run.returncode == 1,
                        f'exit {run.returncode}')
        failed += check('guard: message names both symbols',
                        'ppu_lifter.py: error: --cpp-nonlocal-jumps lowered no setjmp or longjmp'
                        in run.stderr and '0x00001100 (.setjmp)' in run.stderr
                        and '0x00001180 (.longjmp)' in run.stderr, run.stderr[-400:])

        run, text = lift(root, 'plain', elf, functions, lowering=False)
        failed += check('no --cpp-nonlocal-jumps: names only',
                        run.returncode == 0 and '/* .longjmp */' in text
                        and 'throw ppu_nonlocal_jump{' not in text and '_nonlocal_sp_' not in text,
                        run.stderr[-400:])
        lonely, lonely_functions = root/'lonely.elf', root/'lonely.json'
        lonely_functions.write_text(json.dumps(elf_fixture(lonely, SYMBOLS, LONELY_CODE)),
                                    encoding='utf-8')
        run, text = lift(root, 'lonely', lonely, lonely_functions)
        failed += check('longjmp without setjmp caller: exit 0 and a throw line',
                        run.returncode == 0 and '/* .longjmp */' in text
                        and 'throw ppu_nonlocal_jump{' in body(text, 'func_00001180')
                        and '_nonlocal_sp_' not in text, run.stderr[-400:])

        run, text = lift(root, 'bare', bare, functions)
        failed += check('ELF without symbol table: exit 0 and count 0',
                        run.returncode == 0
                        and '  names: 0 function symbols from the ELF symbol table\n' in run.stdout,
                        run.stdout[-200:] + run.stderr[-200:])
    print(f'{failed} failed')
    return int(failed != 0)


if __name__ == '__main__':
    raise SystemExit(main())
