"""Exercise extracted real PPU/SPU commit paths with authored VM/lock scaffolding."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--compiler',default=os.environ.get('CC') or
               (shutil.which('clang-cl') if os.name=='nt' else None) or shutil.which('cc'))
p.add_argument('--mutants',action='store_true')
p.add_argument('--syntax',action='store_true',help='Also compile the full production SPU translation unit')
p.add_argument('--output',type=Path,default=Path('build/native-atomic-observer-tests'))
a=p.parse_args()
assert a.compiler,'C17/C++17 compiler required'
msvc=Path(a.compiler).stem.lower() in ('cl','clang-cl')
out=(ROOT/a.output).resolve();out.mkdir(parents=True,exist_ok=True)
env={k:v for k,v in os.environ.items() if not k.startswith(('SPU_','SPURS_','PPU_','YDKJ_','LBP_','GH3_'))}
paths=[ROOT/'runtime/ppu/ppu_loader.cpp',ROOT/'runtime/spu/spu_channels.c',
       ROOT/'include/ps3emu/native_atomic_observer.h']
ppu,spu,header=[x.read_text() for x in paths]

def function(text,signature):
    start=text.index(signature);opening=text.index('{',start)
    # Replace comments/string contents with spaces, preserving all positions.
    clean=re.sub(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                 lambda m:' '*len(m[0]),text)
    depth=1;i=opening+1
    while depth:
        depth += (clean[i]=='{')-(clean[i]=='}');i+=1
    return text[start:i]

def install(text,kind):
    return text.split('/* PS3_ATOMIC_OBSERVER_'+kind+'_BEGIN */',1)[1].split(
        '/* PS3_ATOMIC_OBSERVER_'+kind+'_END */',1)[0]

ppu_body=install(ppu,'PPU')+'\n'+function(ppu,'extern "C" int ppu_stwcx32(')+'\n'+function(ppu,'extern "C" int ppu_stdcx64(')
spu_body=install(spu,'SPU')+'\n'+function(spu,'static int spu_mfc_atomic(')

def execute(command,directory,label):
    (directory/(label+'-command.json')).write_text(json.dumps(command,indent=2))
    r=subprocess.run(command,cwd=directory,env=env,capture_output=True,text=True,timeout=90)
    (directory/(label+'.log')).write_text(r.stdout+r.stderr)
    return r

def run(name,enabled,ppu_source=ppu_body,spu_source=spu_body,api=header):
    directory=out/name;directory.mkdir(parents=True,exist_ok=True)
    local=directory/'include/ps3emu';local.mkdir(parents=True,exist_ok=True)
    (local/'native_atomic_observer.h').write_text(api)
    ppufile=directory/'ppu.cpp';spufile=directory/'spu.c'
    template=(ROOT/'tests/native_atomic_observer_ppu.cpp').read_text()
    assert template.count('/* FIXTURE_PRODUCTION_INSERT */')==1
    ppufile.write_text(template.replace('/* FIXTURE_PRODUCTION_INSERT */',ppu_source))
    spufile.write_text('#include "native_atomic_observer_support.h"\n'+spu_source+
        '\nint issue219_atomic(spu_context*c,uint32_t cmd){return spu_mfc_atomic(c,cmd);}\n')
    files=[ppufile,spufile,ROOT/'tests/native_atomic_observer_driver.cpp'];objects=[]
    for i,f in enumerate(files):
        obj=directory/(str(i)+('.obj' if msvc else '.o'));objects.append(obj)
        cpp=f.suffix=='.cpp'
        command=[a.compiler,str(f)]
        if msvc:
            command+=['/nologo','/O2','/UNDEBUG','/std:c++17' if cpp else '/std:c17','/c',
                '/DPS3_ATOMIC_OBSERVATION='+str(enabled),'/I'+str(directory/'include'),
                '/I'+str(ROOT/'include'),'/I'+str(ROOT),'/I'+str(ROOT/'tests'),'/Fo'+str(obj)]
        else:
            command+=['-O2','-UNDEBUG','-std=c++17' if cpp else '-std=c17','-c',
                '-DPS3_ATOMIC_OBSERVATION='+str(enabled),'-I'+str(directory/'include'),
                '-I'+str(ROOT/'include'),'-I'+str(ROOT),'-I'+str(ROOT/'tests'),'-o',str(obj)]
        r=execute(command,directory,'compile-'+str(i));assert r.returncode==0,r.stdout+r.stderr
    exe=directory/('fixture.exe' if os.name=='nt' else 'fixture')
    command=[a.compiler,*map(str,objects)]
    command+=['/nologo','/Fe'+str(exe)] if msvc else ['-o',str(exe)]
    r=execute(command,directory,'link');assert r.returncode==0,r.stdout+r.stderr
    return execute([str(exe)],directory,'run')

for enabled in (0,1):
    if a.syntax:
        command=[a.compiler,str(paths[1])]
        command+=(['/nologo','/std:c17','/Zs','/DPS3_ATOMIC_OBSERVATION='+str(enabled),
                   '/I'+str(ROOT/'include')] if msvc else
                  ['-std=gnu17','-D_GNU_SOURCE','-fsyntax-only','-DPS3_ATOMIC_OBSERVATION='+str(enabled),
                   '-I'+str(ROOT/'include')])
        r=execute(command,out,'production-syntax-'+str(enabled))
        assert r.returncode==0,r.stdout+r.stderr
    r=run('enabled-'+str(enabled),enabled)
    assert r.returncode==0,r.stdout+r.stderr
    assert 'PASS actual PPU/SPU atomic observations' in r.stdout
    print('PASS actual atomic paths, compile option',enabled)

rejected=[]
if a.mutants:
    mutants={
        'failed-store-completed':(ppu_body,spu_body,header.replace(
            'PS3_ATOMIC_ATTEMPTED | (ok ? PS3_ATOMIC_COMMITTED : 0)',
            'PS3_ATOMIC_ATTEMPTED | PS3_ATOMIC_COMMITTED'),
            '!!(last.flags&PS3_ATOMIC_COMMITTED)==committed'),
        'late-payload-read':(ppu_body,spu_body.replace(
            'if (observer) memcpy(observation.after, mem, MFC_ATOMIC_LINE);',
            '/* Deliberately omit the locked snapshot. */').replace(
            'if (observer) ps3_atomic_observation_end(&observation, observer,',
            'if (observer) memcpy(observation.after, mem, MFC_ATOMIC_LINE);\n'
            '        if (observer) ps3_atomic_observation_end(&observation, observer,'),header,
            'all(last.after,128,success?0x22:0x11)'),
        'hidden-invalid-clock':(ppu_body,spu_body,header.replace(
            'e->flags |= PS3_ATOMIC_CLOCK_INVALID;', 'e->flags |= 0;'),
            'last.flags&PS3_ATOMIC_CLOCK_INVALID'),
        'failed-spu-store-completed':(ppu_body,spu_body.replace(
            'PS3_ATOMIC_ATTEMPTED | (ctx->atomic_stat == 0 ? PS3_ATOMIC_COMMITTED : 0)',
            'PS3_ATOMIC_ATTEMPTED | PS3_ATOMIC_COMMITTED'),header,
            '!!(last.flags&PS3_ATOMIC_COMMITTED)==committed'),
    }
    endblock='''        #if PS3_ATOMIC_OBSERVATION
        if (observer) ps3_atomic_observation_end(&observation, observer,
            PS3_ATOMIC_ATTEMPTED | (ctx->atomic_stat == 0 ? PS3_ATOMIC_COMMITTED : 0));
        #endif'''
    anchor='        spu_lockline_unlock();\n'+endblock
    assert spu_body.count(anchor)==1
    mutants['callback-under-lock']=(ppu_body,spu_body.replace(anchor,
        endblock+'\n        spu_lockline_unlock();'),header,'!fixture_lock_held')
    host_call='if (g_spu_host_line_commit_hook) g_spu_host_line_commit_hook(ea & ~127u);'
    assert spu_body.count(host_call)==2
    mutants['missing-host-wakeup']=(ppu_body,spu_body.replace(host_call,
        '/* Deliberately omit host wakeup. */'),header,
        'host_notifications==(unsigned)success')
    mutants['host-wakeup-under-lock']=(ppu_body,spu_body.replace(anchor,
        '        '+host_call+'\n'+anchor,1),header,'!fixture_lock_held')
    for name,(pp,sp,api,expected) in mutants.items():
        assert (pp,sp,api)!=(ppu_body,spu_body,header),name
        r=run(name,1,pp,sp,api)
        assert r.returncode!=0 and expected in r.stdout+r.stderr,(name,r.stdout,r.stderr)
        rejected.append(name);print('PASS rejected',name,'at its specific observation assertion')

(out/'result.json').write_text(json.dumps({
    'compiler':a.compiler,'full_spu_syntax':a.syntax,
    'sources':{str(x.relative_to(ROOT)):hashlib.sha256(x.read_bytes()).hexdigest() for x in paths+[
        ROOT/'tests/native_atomic_observer_driver.cpp',ROOT/'tests/native_atomic_observer_ppu.cpp',
        ROOT/'tests/native_atomic_observer_support.h',Path(__file__).resolve()]},
    'mutants':rejected,'scope':'Extracted production commit functions; authored VM/lock/coherency dependencies. '
    'No full coherence proof, game binding, timeline, overhead measurement or FPS claim.'},indent=2))
