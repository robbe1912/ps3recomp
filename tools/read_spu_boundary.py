#!/usr/bin/env python3
"""Decode private, bounded native SPU boundary rings; reject incomplete slots."""
import json, struct, sys
from pathlib import Path

def read_ring(path):
    data=Path(path).read_bytes()
    assert len(data)>=4096, 'Missing boundary header'
    h=json.loads(data[:4096].split(b'\0',1)[0])
    assert h['format']==1 and h['endian']==0x01020304
    size=h['record_size'];cap=h['capacity'];commit=h['commit_offset']
    assert 16<=cap<=1024 and 40<=h['gpr_offset'] and h['gpr_offset']+2048<=h['ls_offset']
    assert h['ls_offset']+4096<=commit and commit+8==size<=8192
    assert 0<h['ls_length']<=4096 and 0<=h['ls_start']<=0x40000-h['ls_length']
    assert len(data)<=4096+cap*size, 'Oversized ring'
    rows=[];invalid=[]
    for slot in range((len(data)-4096+size-1)//size):
        r=data[4096+slot*size:4096+(slot+1)*size]
        if not any(r):continue
        if len(r)!=size:invalid.append(slot);continue
        seq,tick,owner,tid,pc,insn,policy=struct.unpack_from('<QQQIIII',r)
        end=struct.unpack_from('<Q',r,commit)[0]
        if not seq or seq!=end or (seq-1)%cap!=slot:invalid.append(slot);continue
        assert owner==h['context'] and pc<0x40000 and pc%4==0
        rows.append(dict(sequence=seq,tick=tick,context=owner,thread=tid,pc=pc,instruction=insn,
                         policy=policy,gpr=r[h['gpr_offset']:h['gpr_offset']+2048],
                         ls=r[h['ls_offset']:h['ls_offset']+h['ls_length']]))
    rows.sort(key=lambda r:r['sequence'])
    sequences=[r['sequence'] for r in rows]
    assert len(sequences)==len(set(sequences)), 'Duplicate committed sequence'
    assert not sequences or sequences[-1]-sequences[0]<cap, 'Invalid retained sequence range'
    gaps=[] if not rows else sorted(set(range(sequences[0],sequences[-1]+1))-set(sequences))
    return dict(header=h,rows=rows,invalid_slots=invalid,gaps=gaps,
                overwritten_before_first=sequences[0]-1 if rows else 0,
                qualification='Selected pre-execution observations; LS copy is not an all-writer atomic snapshot. No retired-instruction or submission-identity claim.')

if __name__=='__main__':
    result=read_ring(sys.argv[1])
    for r in result['rows']:
        r['gpr']=r['gpr'].hex();r['ls']=r['ls'].hex()
    print(json.dumps(result,indent=2))
    raise SystemExit(2 if result['invalid_slots'] or result['gaps'] else 0)
