"""Compile the production SPU reserved-line bitmap and check the inline test.

The fixture `tests/spu_coh_bitmap_harness.c` includes the production header and
links the production source. Each mutant changes one expression of the header
or the source and must make the fixture fail."""
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'runtime/spu/spu_coherency.h'
SOURCE = ROOT / 'runtime/spu/spu_coherency.c'
FIXTURE = ROOT / 'tests/spu_coh_bitmap_harness.c'

MUTANTS = [
    ('line shift 7 to 6', HEADER,
     '#define SPU_COH_LINE_SHIFT  7 ', '#define SPU_COH_LINE_SHIFT  6 '),
    ('line shift 7 to 8', HEADER,
     '#define SPU_COH_LINE_SHIFT  7 ', '#define SPU_COH_LINE_SHIFT  8 '),
    ('byte index >> 3 to >> 2', HEADER,
     'uint32_t line = addr >> SPU_COH_LINE_SHIFT;\n    return (g_spu_coh_bitmap[line >> 3]',
     'uint32_t line = addr >> SPU_COH_LINE_SHIFT;\n    return (g_spu_coh_bitmap[line >> 2]'),
    ('bit mask & 7 to & 3', HEADER, '(line & 7)) & 1u;', '(line & 3)) & 1u;'),
    ('bit order from the high end', HEADER, '(line & 7)) & 1u;', '(7 - (line & 7))) & 1u;'),
    ('armed check removed', SOURCE,
     '    if (!s_coh_armed) return 0;\n    return spu_coh_bitmap_test(addr);',
     '    return spu_coh_bitmap_test(addr);'),
]


def compiler():
    found = os.environ.get('MV_CLANG') or os.environ.get('CC') or shutil.which('clang-cl') or shutil.which('clang') or shutil.which('cc')
    if not found:
        raise RuntimeError('No C compiler found; set MV_CLANG or CC')
    return found


def build(run, cc, msvc_style, header, source):
    run.mkdir(parents=True, exist_ok=True)
    # The directory of the given header is first, so a mutant header wins.
    include = [str(header.parent), str(ROOT / 'runtime/spu'), str(ROOT / 'include'), str(ROOT / 'runtime'), str(ROOT / 'libs')]
    exe = run / ('check.exe' if os.name == 'nt' else 'check')
    if msvc_style:
        common = [cc, '/nologo', '/O2', '/UNDEBUG', '/D_CRT_SECURE_NO_WARNINGS']
        common += ['/I' + path for path in include]
        commands = [common + ['/c', str(source), '/Fo' + str(run / 'coh.obj')],
                    common + ['/I' + str(run.parent), str(FIXTURE), str(run / 'coh.obj'), '/Fe' + str(exe)]]
    else:
        common = [cc, '-std=c11', '-O2', '-UNDEBUG', '-D_CRT_SECURE_NO_WARNINGS']
        common += ['-I' + path for path in include]
        commands = [common + ['-c', str(source), '-o', str(run / 'coh.o')],
                    common + ['-I' + str(run.parent), str(FIXTURE), str(run / 'coh.o'), '-o', str(exe)]]
    for command in commands:
        done = subprocess.run(command, capture_output=True, text=True, errors='replace')
        (run / 'build.log').write_text(done.stdout + done.stderr)
        if done.returncode:
            print(done.stdout + done.stderr)
            return None
    return exe


def run_fixture(exe):
    done = subprocess.run([str(exe)], capture_output=True, text=True, errors='replace')
    return done.returncode, done.stdout + done.stderr


def main():
    cc = compiler()
    msvc_style = Path(cc).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')
    base = ROOT / 'build/self-tests/spu-coh-bitmap'
    out = base / 'production'
    shutil.rmtree(out, ignore_errors=True)
    exe = build(out, cc, msvc_style, HEADER, SOURCE)
    if exe is None:
        return 1
    code, text = run_fixture(exe)
    (out / 'run.log').write_text(text)
    print(text.strip())
    if code or 'spu_coh_bitmap: PASS' not in text:
        return 1
    for name, path, old, new in MUTANTS:
        source = path.read_text()
        if source.count(old) != 1:
            raise RuntimeError('Mutant target is not unique: ' + name)
        out = base / 'mutant'
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True, exist_ok=True)
        (out / path.name).write_text(source.replace(old, new))
        # The header is found through the include path; the source is compiled by file.
        other = SOURCE if path is HEADER else HEADER
        exe = build(out, cc, msvc_style, out / HEADER.name, out / SOURCE.name if path is SOURCE else other)
        if exe is None:
            print('mutant did not compile: ' + name)
            return 1
        code, text = run_fixture(exe)
        (out / 'run.log').write_text(text)
        if code == 0:
            print('mutant accepted: ' + name)
            return 1
        print('mutant rejected: ' + name)
    print('PASS tests/check_spu_coh_bitmap.py')
    return 0


if __name__ == '__main__':
    sys.exit(main())
