"""Live deferred-pointer observation; authored memory only, no guest inputs."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def main(mode='wait'):
    if os.name != 'nt':
        print('SKIP: Windows debug API fixture'); return
    from windows_data_watch import process_creation_time
    out = ROOT/'temp/checks'/('data-follow-'+mode+'-'+str(time.time_ns()))
    out.mkdir(parents=True)
    source = out/'writer.c'; exe = out/'writer.exe'
    source.write_text(r'''
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
__declspec(align(16)) volatile uint64_t memory[32];
__declspec(align(8)) struct { volatile uint32_t guard, pointer; } metadata;
#define CELLS (&memory[8])
static int mode;
static DWORD WINAPI worker(void *unused) {
    (void)unused; CELLS[0]=2; CELLS[1]=4;
    if (mode==2) metadata.pointer=0x80000000; /* BE offset 128: lifetime change */
    else CELLS[0]=0x000000000000C07FULL; /* BE quiet NaN */
    return 0;
}
int main(int argc, char **argv) {
    if (argc!=3) return 2;
    mode=atoi(argv[2]);
    if (mode==1) { metadata.guard=0x78563412; metadata.pointer=0x40000000; }
    printf("%llu %llu %llu\n", (unsigned long long)(uintptr_t)&metadata.pointer,
           (unsigned long long)(uintptr_t)memory, (unsigned long long)(uintptr_t)&metadata.guard);
    fflush(stdout);
    for (int n=0; GetFileAttributesA(argv[1])==INVALID_FILE_ATTRIBUTES; ++n) {
        if (n>2000) return 3; Sleep(10);
    }
    if (mode!=1) {
        metadata.guard=1; metadata.pointer=0x40000000;
        CELLS[0]=0x000000000000C07FULL; /* invalid data behind a mismatched guard */
        metadata.pointer=0; CELLS[0]=0;
        metadata.guard=0x78563412; metadata.pointer=0x40000000;
    }
    CELLS[0]=1; CELLS[1]=3;
    HANDLE thread=CreateThread(NULL,0,worker,NULL,0,NULL);
    if (!thread) return 4;
    WaitForSingleObject(thread,INFINITE); CloseHandle(thread);
    Sleep(200); CELLS[0]=5; CELLS[1]=6;
    printf("FINAL %llu %llu\n", (unsigned long long)CELLS[0], (unsigned long long)CELLS[1]);
    return CELLS[0]!=5 || CELLS[1]!=6;
}
''')
    if mode=='reserved':
        source.write_text(source.read_text().replace(
            '__declspec(align(8)) struct { volatile uint32_t guard, pointer; } metadata;',
            'struct Meta { volatile uint32_t guard, pointer; };\nstatic struct Meta *meta;\n#define metadata (*meta)').replace(
            'mode=atoi(argv[2]);',
            'mode=atoi(argv[2]); meta=VirtualAlloc(NULL,4096,MEM_RESERVE,PAGE_READWRITE); if (!meta) return 6;').replace(
            'if (mode!=1) {',
            'if (!VirtualAlloc(meta,4096,MEM_COMMIT,PAGE_READWRITE)) return 7;\n    if (mode!=1) {'))
    cmd = [os.environ.get('MV_CLANG', 'clang-cl'), '/nologo', '/O1', '/Zi', str(source),
           '/Fe'+str(exe), '/Fo'+str(out/'writer.obj'), '/link', 'kernel32.lib', '/PDB:'+str(out/'writer.pdb')]
    build = subprocess.run(cmd, cwd=out, capture_output=True, text=True)
    (out/'build.log').write_text(build.stdout+build.stderr)
    assert build.returncode == 0, build.stdout+build.stderr
    go = out/'go.txt'
    child = subprocess.Popen([str(exe), str(go), str({'wait':0, 'initial':1, 'changed':2, 'reserved':3}[mode])],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                             creationflags=subprocess.CREATE_NO_WINDOW)
    observer = None
    try:
        pointer, base, guard = map(int, child.stdout.readline().split())
        owner = dict(pid=child.pid, create_time=process_creation_time(int(child._handle)),
                     exe=str(exe), exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest())
        manifest = out/'owner.json'; manifest.write_text(json.dumps(owner))
        watch = out/'watch'
        cmd = [sys.executable, str(ROOT/'tools/windows_data_watch.py'), '--owner', str(manifest),
               '--out', str(watch), '--seconds', '15', '--events', '100', '--stop-nonfinite-be-f32',
               '--follow-be32', hex(pointer), '--follow-base', hex(base),
               '--follow-offset', '0', '--follow-offset', '8', '--follow-guard', hex(guard)+':12345678']
        observer = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
        deadline = time.monotonic()+10
        while not (watch/'ready.json').exists():
            if observer.poll() is not None: raise AssertionError(observer.communicate())
            assert time.monotonic()<deadline, 'Debugger not ready'
            time.sleep(.01)
        ready = json.loads((watch/'ready.json').read_text())
        assert ready['follow_armed'] == (mode=='initial'), ready
        assert ready['follow_source_mapped'] == (mode!='reserved'), ready
        if mode=='reserved': assert ready['values']==[None], ready
        go.write_text('authored pointer and data writes')
        stdout, stderr = observer.communicate(timeout=20)
        result = json.loads((watch/'final.json').read_text())
        assert not result['cleanup_errors'], result
        events = [json.loads(l) for l in (watch/'events.jsonl').read_text().splitlines()]
        armed = [e for e in events if e['kind']=='follow_armed']
        assert len(armed)==1 and armed[0]['pointer']=='0x40', armed
        assert armed[0]['addresses']==[pointer & ~7, base+64, base+72], armed
        assert armed[0]['values'][1:]==['0000000000000000']*2, armed
        writes = [e for e in events if e['kind']=='write']
        assert len({e['tid'] for e in writes})==2, 'New writer thread missed'
        for row, (slot, value) in zip(writes, [(1,1), (2,3), (1,2), (2,4)]):
            assert row['mask']==1 << slot, row
            assert int.from_bytes(bytes.fromhex(row['values'][slot]), 'little')==value, row
        if mode=='changed':
            assert observer.returncode != 0 and 'Follow pointer changed after arming' in result['failure'], (stdout, stderr, result)
            assert len(writes)==4 and result['dumps']==0, result
        else:
            assert observer.returncode==0 and result['failure'] is None, (stdout, stderr, result)
            assert result['reason']=='nonfinite BE float' and len(writes)==5 and result['dumps']==1, result
            assert writes[-1]['mask']==2 and writes[-1]['values'][1]=='7fc0000000000000'
            assert writes[-1]['nonfinite_be_f32'] and not any(w.get('dump') for w in writes[:-1])
        final, err = child.communicate(timeout=10)
        assert child.returncode==0 and 'FINAL 5 6' in final, (child.returncode, final, err)
        (out/'result.json').write_text(json.dumps(dict(passed=True, mode=mode, final=result,
            tool_sha256=hashlib.sha256((ROOT/'tools/windows_data_watch.py').read_bytes()).hexdigest()), indent=2))
        print('PASS: follow '+mode+'; '+str(out))
    finally:
        if observer and observer.poll() is None: observer.communicate(timeout=25)
        if child.poll() is None: child.terminate(); child.communicate(timeout=5)


if __name__ == '__main__':
    for mode in ('wait', 'initial', 'changed', 'reserved'): main(mode)
