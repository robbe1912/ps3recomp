"""Compile the real float helpers and reject five scope-state mutants."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]
SPU=ROOT/'runtime/spu'

def main():
    compiler=os.environ.get('MV_CLANG') or os.environ.get('CC') or shutil.which('clang-cl' if os.name=='nt' else 'cc')
    assert compiler,'Set MV_CLANG or CC and configure the compiler environment'
    out=ROOT/'temp/checks'/('spu-float-scope-'+str(time.time_ns()))
    out.mkdir(parents=True)
    records=[]
    def run(command):
        result=subprocess.run(list(map(str,command)),capture_output=True,text=True,errors='replace',timeout=30)
        records.append(dict(command=list(map(str,command)),returncode=result.returncode,stdout=result.stdout,stderr=result.stderr))
        (out/'commands.json').write_text(json.dumps(records,indent=2))
        return result
    mutants=[('scope',None,None,None),
             ('held-host','spu_float_simd.h',b'    _mm_setcsr((g_spu_sf_host_csr',b'    if (0) _mm_setcsr((g_spu_sf_host_csr'),
             ('not-restored','spu_float_simd.h',b'if (held) _mm_setcsr(held);',b'(void)held;'),
             ('outside-scope','spu_float_simd.h',b' || !g_spu_sf_simd_scope) return 0;',b') return 0;'),
             ('nested-overwrite','spu_float_simd.c',b'if (!g_spu_sf_simd_scope) g_spu_sf_host_csr=csr;',b'if (!spu_sf_csr_is_spu(csr)) g_spu_sf_host_csr=csr;'),
             ('spu-caller-not-captured','spu_float_simd.c',b'if (!g_spu_sf_simd_scope) g_spu_sf_host_csr=csr;',b'if (!g_spu_sf_simd_scope && !spu_sf_csr_is_spu(csr)) g_spu_sf_host_csr=csr;')]
    for name,file,old,new in mutants:
        private=out/name
        private.mkdir()
        for source in ['spu_helpers.h','spu_float_simd.h','spu_float_simd.c']:
            data=(SPU/source).read_bytes()
            if source==file:
                assert data.count(old)==1,(name,source)
                data=data.replace(old,new)
            (private/source).write_bytes(data)
        exe=private/('test.exe' if os.name=='nt' else 'test')
        includes=[private,ROOT,ROOT/'include',SPU]
        sources=[SPU/'tests/test_spu_float_scope.c',private/'spu_float_simd.c']
        if os.name=='nt':
            command=[compiler,'/nologo','/std:c17','/O1','/fp:strict','/D_CRT_SECURE_NO_WARNINGS',*['/I'+str(x) for x in includes],*sources,'/Fe'+str(exe),'/Fo'+str(private)+os.sep]
        else:
            command=[compiler,'-std=c17','-O1','-frounding-math','-ffp-contract=off',*['-I'+str(x) for x in includes],*sources,'-lm','-o',exe]
        result=run(command)
        assert result.returncode==0,(name,result.stdout,result.stderr)
        result=run([exe])
        assert result.returncode==(0 if name=='scope' else 1),(name,result.stdout,result.stderr)
        assert ('scope failures=0' in result.stdout)==(name=='scope'),(name,result.stdout)
    print('PASS real float scope and five compiled mutants:',out)

if __name__=='__main__':
    main()
