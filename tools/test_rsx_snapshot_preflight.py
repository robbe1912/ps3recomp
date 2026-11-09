"""Exercise actual snapshot provider admission with authored CPU state and copies."""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'temp/checks' / ('snapshot-preflight-' + str(time.time_ns()))
OUT.mkdir(parents=True)
source = ROOT / 'libs/video/rsx_cpu_surface.inc'
text = source.read_text()
start = text.index('static int cpu_surface_snapshot(')
end = text.index('\nstatic void cpu_surface_service(', start)
body = text[start:end]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <crtdbg.h>
#endif
#include "rsx_cpu_surface_snapshot.h"
typedef uint32_t u32;
#define CPU_SURFACE_CAPACITY 2
#define E_FAIL 1
typedef struct {
    void *res;
    u32 cpu_address, cpu_pitch, h;
    uint64_t gpu_generation, cpu_generation;
} OffRT;
typedef struct { int in_begin_end; } State;
static State state;
static struct {
    OffRT off_rt[CPU_SURFACE_CAPACITY];
    u32 draw_count;
    const State *current_rsx_state;
} s_d3d;
static int s_cpu_surface_recording;
static struct { unsigned matched; } *s_cpu_surface_profile_active;
static uint8_t memory[0x2000];
uint8_t *vm_base = memory;
static unsigned copies;
static void cpu_surface_fail(const char *message, int error) {
    fprintf(stderr, "%s: %d\n", message, error);
    abort();
}
static void cpu_surface_copy(OffRT *target) {
    assert(target == &s_d3d.off_rt[0]);
    ++copies;
    memory[0x1007] = 0x31;
    memory[0x1008] = 0x72;
}
// PRODUCTION
static void rejected(void) {
    const VmReadSnapshotRange range = {0x1007, 2, 0};
    uint8_t output[] = {0xa5, 0x5a};
    unsigned before = copies;
    assert(cpu_surface_snapshot(&range, 1, output, 2) == VM_READ_SNAPSHOT_UNAVAILABLE);
    assert(copies == before && output[0] == 0xa5 && output[1] == 0x5a);
}
int main(int argc, char **argv) {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    (void)argv;
    s_d3d.off_rt[0] = (OffRT){memory, 0x1000, 8, 3, 2, 1};
    s_d3d.current_rsx_state = &state;
    if (argc > 1) {
        const VmReadSnapshotRange range = {0x1007, 2, 0};
        uint8_t output[2];
        s_cpu_surface_recording = 1;
        cpu_surface_snapshot(&range, 1, output, 2);
        return 0;
    }
    s_d3d.draw_count = 1;
    rejected();
    s_d3d.draw_count = 0;
    state.in_begin_end = 1;
    rejected();
    state.in_begin_end = 0;
    s_d3d.current_rsx_state = NULL;
    rejected();
    s_d3d.current_rsx_state = &state;
    const VmReadSnapshotRange range = {0x1007, 2, 0};
    uint8_t output[2] = {0};
    assert(cpu_surface_snapshot(&range, 1, output, 2) == VM_READ_SNAPSHOT_OK);
    assert(copies == 1 && output[0] == 0x31 && output[1] == 0x72);
    puts("PASS snapshot CPU preflight, exact copy and unchanged rejected staging");
}
'''
compiler = os.environ.get('MV_CLANG') or shutil.which('clang-cl' if os.name == 'nt' else 'clang') or shutil.which('cc')
assert compiler, 'Prepare the compiler environment first'
msvc = Path(compiler).name.lower().startswith('clang-cl')
report = dict(qualification=__doc__, source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(), results=[])

def check(name, code, expected):
    path = OUT / (name + '.c')
    path.write_text(fixture.replace('// PRODUCTION', code))
    exe = OUT / (name + ('.exe' if os.name == 'nt' else ''))
    includes = [ROOT / 'include', ROOT / 'libs/video']
    if msvc:
        command = [compiler, '/nologo', '/std:c17', '/O2', '/MDd', '/UNDEBUG',
                   *['/I' + str(p) for p in includes], str(path), '/Fe' + str(exe),
                   '/Fo' + str(OUT / (name + '.obj'))]
    else:
        command = [compiler, '-std=c17', '-O2', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                   *[flag for p in includes for flag in ('-I', str(p))], str(path), '-o', str(exe)]
    built = subprocess.run(command, text=True, capture_output=True, timeout=90)
    report['results'].append(dict(command=command, returncode=built.returncode,
                                  stdout=built.stdout, stderr=built.stderr))
    assert built.returncode == 0, built.stderr
    for args, success in [([], expected), (['recording'], False)]:
        ran = subprocess.run([str(exe), *args], text=True, capture_output=True, timeout=15)
        report['results'].append(dict(command=[str(exe), *args], returncode=ran.returncode,
                                      stdout=ran.stdout, stderr=ran.stderr))
        (OUT / 'result.json').write_text(json.dumps(report, indent=2))
        if args:
            assert 'CPU snapshot requires pending recorded surface writes' in ran.stderr
        assert (ran.returncode == 0) == success, (name, args, ran.stdout, ran.stderr)

check('production', body, True)
for name, old, new in [
    ('pending-draw', 's_d3d.draw_count)', '0)'),
    ('open-primitive', 's_d3d.current_rsx_state->in_begin_end ||', ''),
    ('missing-state', '!s_d3d.current_rsx_state ||', '0 ||'),
]:
    mutant = body.replace(old, new)
    assert mutant != body, name
    check(name, mutant, False)
print('PASS actual CPU provider preflight and three source mutants:', OUT)
