"""Actual SPU helpers versus an independent exact-rational ISA value model.

IBM SPU ISA v1.2, section 9.1, FA/FS/FM/FMA/FMS/FNMS and FI.
No guest bytes, no RPCS3 implementation. Does not test FPSCR status flags.
"""
import argparse
from fractions import Fraction
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]
SOURCE='https://arcb.csc.ncsu.edu/~mueller/cluster/ps3/SDK3.0/docs/arch/SPU_ISA_v1.2_27Jan2007_pub.pdf'

def pow2(e):
    return Fraction(1 << e) if e>=0 else Fraction(1,1 << -e)

def value(raw):
    exponent=(raw >> 23) & 255
    if not exponent: return Fraction(0)
    magnitude=Fraction((raw & 0x7fffff) | 0x800000)*pow2(exponent-150)
    return -magnitude if raw >> 31 else magnitude

def pack(x):
    if not x: return 0
    sign=0x80000000 if x<0 else 0
    x=abs(x); n=x.numerator; d=x.denominator
    e=n.bit_length()-d.bit_length()
    if x < pow2(e): e-=1
    biased=e+127
    if biased<1: return 0
    if biased>255: return sign | 0x7fffffff
    mantissa=x*pow2(23-e)
    significand=mantissa.numerator // mantissa.denominator
    assert 0x800000<=significand<=0xffffff
    return sign | biased << 23 | (significand & 0x7fffff)

def reference(op,a,b,c):
    if op==7:
        assert (a & 0x7f800000)==0
        return 0
    if op==6:
        # FI's RB is packed base/step, not an ordinary float operand.
        base=1+Fraction(b & 0x7ffc00,1 << 23)
        step=Fraction(b & 0x3ff,1 << 13)
        y=Fraction(a & 0x7ffff,1 << 19)
        result=(base-step*y)*pow2(((b >> 23) & 255)-127)
        return pack(-result if b >> 31 else result)
    av,bv,cv=map(value,(a,b,c))
    return pack([lambda:av+bv,lambda:av-bv,lambda:av*bv,
                 lambda:av*bv+cv,lambda:av*bv-cv,lambda:cv-av*bv][op]())

def cases():
    rng=random.Random(0x116)
    edges=[0,0x80000000,1,0x007fffff,0x80000001,0x807fffff,
           0x00800000,0x80800000,0x00800001,0x3eaaaaab,0x3f000000,
           0x3f800000,0xbf800000,0x3f800001,0x40000000,0x7f000000,
           0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,
           0x7fb50160,0x7ff50000,0x7fffffff,0xffffffff]
    for op in range(7):
        group=[]
        for a in edges:
            for b in edges:
                group.append((a,b,edges[(edges.index(a)*7+edges.index(b)*3)%len(edges)]))
        if op in (3,4,5):
            # Non-saturated product and subnormal product must survive until add.
            group += [(0x7fffffff,0x40000000,0xffffffff),
                      (0x00800000,0x3f000000,0x00800000),
                      (0x3f800001,0x3f800001,0xbf800002)]
        if op==6:
            # All packed exponent values, both signs, base/step endpoints.
            group += [(a,(s << 31)|(e << 23)|f,0)
                      for e in range(256) for s in (0,1)
                      for f in (0,0x3ff,0x7ffc00,0x7fffff)
                      for a in (0,0x7ffff)]
        group += [(rng.getrandbits(32),rng.getrandbits(32),rng.getrandbits(32)) for _ in range(4096)]
        while len(group)%4: group.append((0,0,0))
        for i in range(0,len(group),4): yield op,group[i:i+4]
    yield 7,[(0,0,0),(0x80000000,0,0),(1,0,0),(0x807fffff,0,0)]

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--header-root',type=Path,default=ROOT)
    ap.add_argument('--interpreter',action='store_true');args=ap.parse_args()
    header_root=args.header_root.resolve()
    out=ROOT/'temp/checks'/('spu-float-'+str(time.time_ns()));out.mkdir(parents=True)
    exe=out/('driver.exe' if os.name=='nt' else 'driver')
    compiler=os.environ.get('MV_CLANG') or os.environ.get('CC') or shutil.which('clang-cl' if os.name=='nt' else 'cc')
    assert compiler, 'Set MV_CLANG/CC or install a C compiler'
    source=ROOT/'runtime/spu/tests/spu_float_driver.c'
    if os.name=='nt':
        cmd=[compiler,'/nologo','/O2','/fp:strict','/I'+str(header_root),str(source),'/Fe'+str(exe),'/Fo'+str(out)+'/']
    else:
        cmd=[compiler,'-O2','-std=c11','-ffp-contract=off','-I',str(header_root),str(source),'-o',str(exe)]
    if args.interpreter:
        cmd += [('/D' if os.name=='nt' else '-D')+'SPU_FLOAT_INTERPRETER',str(header_root/'runtime/spu/spu_interp.c')]
    if os.name!='nt':
        cmd += ['-lm'] # GNU linkers resolve libraries after their consuming objects.
    build=subprocess.run(cmd,capture_output=True,text=True)
    (out/'build.log').write_text(build.stdout+build.stderr)
    assert build.returncode==0,build.stdout+build.stderr
    batches=list(cases());stdin=[];expected=[]
    for op,group in batches:
        words=[group[lane][operand] for operand in range(3) for lane in range(4)]
        stdin.append(str(op)+' '+' '.join(f'{word:08x}' for word in words))
        expected.append([reference(op,*triple) for triple in group])
    result=subprocess.run([str(exe)],input='\n'.join(stdin)+'\n',capture_output=True,text=True,check=True)
    lines=result.stdout.splitlines();assert len(lines)==len(batches)
    counts={};examples=[]
    for (op,group),want,line in zip(batches,expected,lines):
        got=[int(word,16) for word in line.split()];assert len(got)==4
        for lane,(actual,correct) in enumerate(zip(got,want)):
            if actual!=correct:
                counts[str(op)]=counts.get(str(op),0)+1
                if len(examples)<20:examples.append(dict(op=op,inputs=[hex(x) for x in group[lane]],actual=hex(actual),expected=hex(correct)))
    report=dict(passed=not counts,source=SOURCE,cases=len(batches)*4,mismatches=counts,examples=examples,
                mode='interpreter' if args.interpreter else 'helpers',
                header_root=str(header_root),helpers_sha256=hashlib.sha256((header_root/'runtime/spu/spu_helpers.h').read_bytes()).hexdigest(),
                limitations=['Value semantics only; no FPSCR status/whole-game acceptance'])
    (out/'result.json').write_text(json.dumps(report,indent=2))
    print(('PASS' if report['passed'] else 'RED')+': '+str(report['cases'])+' actual-helper lanes; mismatches '+str(counts)+'; '+str(out))
    if counts: print(json.dumps(examples[:4]));raise SystemExit(1)

if __name__=='__main__':main()
