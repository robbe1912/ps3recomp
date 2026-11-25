"""Check the production draw cap parser and boundary with mutants."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent


def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    clean = re.sub(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"', lambda m: ' ' * len(m[0]), text)
    depth, end = 1, opening + 1
    while depth:
        depth += (clean[end] == '{') - (clean[end] == '}')
        end += 1
    return text[start:end]


FIXTURE = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint32_t u32;
#define MAX_DRAWS 2048
static const char *value;
static const char *fixture_env(const char *name) { (void)name; return value; }
#define getenv fixture_env
static struct { int initialized; u32 draw_count; } s_d3d;
static int submissions, flushed, admitted;
static void cpu_surface_flush_recorded(int pump) { (void)pump; flushed = 1; }
static void submit_record_capacity(void *context, int present) {
    (void)context;
    if (!flushed || present) exit(4);
    submissions++; s_d3d.draw_count = 0;
}
static int rsx_record_batch_room(u32 *count, u32 capacity, void *context,
                               void (*submit)(void *, int)) {
    (void)context; (void)submit;
    admitted++;
    return *count < capacity;
}
@FUNCTIONS@
int main(int argc, char **argv) {
    value = argc > 1 ? argv[1] : NULL;
    u32 cap = submit_draws_cap();
    value = "512";
    if (submit_draws_cap() != cap) return 5;
    printf("cap=%u\n", cap);
    if (cap != 256) return 0;
    s_d3d.initialized = 1;
    s_d3d.draw_count = 255; ensure_draw_record_capacity();
    if (submissions) return 6;
    s_d3d.draw_count = 256; ensure_draw_record_capacity();
    if (submissions != 1 || s_d3d.draw_count) return 7;
    s_d3d.draw_count = 257; ensure_draw_record_capacity();
    if (submissions != 2 || s_d3d.draw_count) return 8;
    s_d3d.initialized = 0;
    s_d3d.draw_count = 256; ensure_draw_record_capacity();
    return submissions != 2 || admitted != 4;
}
'''


def main():
    text = (ROOT / 'libs/video/rsx_d3d12_backend.c').read_text(encoding='utf-8')
    parser = function(text, 'static u32 submit_draws_cap(void)')
    boundary = function(text, 'static void ensure_draw_record_capacity(void)')
    assert boundary.index('cpu_surface_flush_recorded(0)') < boundary.index('submit_draws_cap()') < boundary.index('rsx_record_batch_room(')
    bodies = parser + '\n' + boundary
    with tempfile.TemporaryDirectory(prefix='submit-draws-') as directory:
        out = Path(directory)
        unit, exe = out / 'test.c', out / ('test.exe' if os.name == 'nt' else 'test')
        def build(source):
            unit.write_text(FIXTURE.replace('@FUNCTIONS@', source), encoding='utf-8')
            if os.name == 'nt':
                command = [os.environ.get('CLANG_CL', 'clang-cl'), '/nologo', str(unit), '/Fe' + str(exe), '/Fo' + str(out) + os.sep]
            else:
                command = [os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', str(unit), '-o', str(exe)]
            subprocess.run(command, cwd=out, check=True, timeout=30)
        def run(value):
            return subprocess.run([str(exe)] + ([] if value is None else [value]), capture_output=True, text=True, timeout=5)
        build(bodies)
        for value, expected in ((None, 0), ('16', 16), ('256', 256), ('0256', 256), ('2048', 2048)):
            result = run(value)
            assert result.returncode == 0 and result.stdout.strip() == f'cap={expected}', (value, result)
        for value in ('', '0', '15', '2049', '4096', '99999999999', '256x', ' 256', '-16', '0x100'):
            result = run(value)
            assert result.returncode == 0 and result.stdout.strip() == 'cap=0' and result.stderr.count('the cap is off') == 1, (value, result)
        mutants = (
            ('ignored', 'if (cap && s_d3d.initialized && s_d3d.draw_count >= cap)', '(void)cap; if (0)', '256'),
            ('exact', 's_d3d.draw_count >= cap', 's_d3d.draw_count == cap', '256'),
            ('uninitialized', 'cap && s_d3d.initialized && ', 'cap && ', '256'),
            ('abort', '        return 0;', '        abort();', '15'),
            ('use-value', '        return 0;', '', '2049'),
        )
        for name, before, after, value in mutants:
            assert before in bodies, name
            build(bodies.replace(before, after, 1))
            result = run(value)
            assert result.returncode != 0 or (value != '256' and result.stdout.strip() != 'cap=0'), name
    print('PASS: parser values, cached selection, draw boundaries, initialization and five mutants')


if __name__ == '__main__':
    main()
