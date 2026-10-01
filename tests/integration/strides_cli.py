#!/usr/bin/env python3
"""Independent affine scalar progression parity for all four scalar-search families."""
import argparse,hashlib,json,os,subprocess,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from oracle_selftest import check_source,run as oracle_run
from model import N
from scalar_batches import Planner
from stride import targets,relations
p=argparse.ArgumentParser()
for name in ('binary','oracle','report'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--hardware',action='store_true');p.add_argument('--backend',choices=('hip','cuda'),default='hip')
p.add_argument('--batch-order',choices=('forward','both-ends'),default='forward')
p.add_argument('--order',choices=('forward','reverse'),default='forward')
p.add_argument('--kernel',choices=('direct','stepped','glv'))
a=p.parse_args();binary=str(a.binary.resolve())
report=dict(batch_order=a.batch_order,order=a.order,selected_kernel=a.kernel,passed=False,cases=[],rejections=0,hardware=a.hardware,oracle_commit=check_source(),binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest())
def run(words,ok=True):
    r=subprocess.run([binary,*map(str,words)],capture_output=True,text=True,timeout=120)
    assert (r.returncode==0)==ok,(words,r.stdout,r.stderr)
    return [json.loads(v) for v in r.stdout.splitlines()] if ok else r
with tempfile.TemporaryDirectory(prefix='kh-strides-cli-') as directory:
    root=Path(directory)
    inventory=run(['devices','--backend',a.backend])[0] if a.hardware else None
    report['inventory']=inventory
    cases=[('single',1,2,2,False),('tail',0x1001,0x1001+33*3-1,3,False),
        ('partial',257,257+17*17-8,17,False),('carry32',(1<<32)-9,(1<<32)-9+19*7,7,False),
        ('wide128',(1<<128)-16,(1<<128)-16+17*((1<<64)+3),(1<<64)+3,False),
        ('wide192',1<<100,(1<<100)+17*((1<<192)+19),(1<<192)+19,False),
        ('order',N-257,N,17,False),('large-step',1,N,N-1,False),
        ('byte-step',257,257+3*256,256,False),('overflow',1,1+257*2,2,True),
        ('no-hit',3,3+17*2,2,False),('max-batch',1<<128,(1<<128)+1048576*11,11,False)]
    if a.order=='reverse':cases.append(('unit',101,138,1,False))
    if a.kernel=='glv':
        if a.order=='forward':cases.append(('unit',101,138,1,False))
        cases.extend([('unit-wide',1<<192,(1<<192)+37,1,False),
                      ('unit-order',N-33,N,1,False),
                      ('unit-max',1<<128,(1<<128)+1048576,1,False)])
    def exercise(mode,kernel,name,begin,end,step,overflow=False,device=0):
        count=1+(end-begin-1)//step
        indices=sorted({1,count,*[1+(1<<bit) for bit in range(20)]}) if count==1048576 else list(range(1,count+1))
        scalars=[begin+((count-i) if a.order=='reverse' else (i-1))*step for i in indices]
        public=oracle_run(a.oracle,[f'pub {k:064x}' for k in scalars])
        # Add an off-lattice target to ensure a contiguous scalar search cannot
        # accidentally satisfy the test. The no-hit case contains only that target.
        # A unit progression has no gaps; its negative target must be outside
        # the interval, including in sparse maximum-batch expectation sets.
        skipped_scalar=begin+1 if step!=1 else end if end<N else begin-1
        skipped=oracle_run(a.oracle,[f'pub {skipped_scalar:064x}'])[0]
        lines,canonical=targets(mode,[skipped] if name=='no-hit' else public+[skipped],overlap=overflow and mode=='vanity')
        file=root/f'{mode}.txt';file.write_text('\n'.join(lines)+'\n')
        bound=2*len({len(line) for line in lines}) if mode=='vanity' else 2 if mode in ('hash160','address') else 1
        words=[mode,'--backend',a.backend,'--range',f'{begin:x}:{end:x}','--stride',f'{step:x}',
               '--targets',file,'--kernel',kernel,'--device',device,'--batch-size','1048576',
               '--candidate-capacity',bound if overflow else 4096]
        if a.order=='reverse':words+=['--order','reverse']
        maximum=1048576 if a.batch_order=='forward' or count>=1048576 else 17
        words[words.index('--batch-size')+1]=maximum
        words+=['--batch-order',a.batch_order]
        if not a.hardware:
            r=run(words,False);assert 'not built' in r.stderr
            return
        rows=run(words);start,summary=rows[0],rows[-1]
        assert start['batch_order']==summary['batch_order']==a.batch_order
        mapped=step!=1 or a.order=='reverse'
        assert start['kernel']==kernel
        if mapped:
            assert start['coordinate_space']==summary['coordinate_space']==('scalar-reverse-index-v1' if a.order=='reverse' else 'scalar-stride-index-v1')
            assert int(start['scalar_begin'],16)==begin and int(start['scalar_end_exclusive'],16)==end and int(start['stride'],16)==step
        else:assert 'coordinate_space' not in start and 'coordinate_space' not in summary
        first,last=(1,count+1) if mapped else (begin,end)
        assert int(start['begin'],16)==first and int(start['end_exclusive'],16)==last
        model=Planner([(first,last)],a.batch_order);limit=maximum;found=[];overflows=0
        for row in rows[1:-1]:
            assert row['type']=='batch'
            low,high=int(row['begin'],16),int(row['end_exclusive'],16)
            assert (low,high)==model.plan(maximum,limit)[:2]
            if row['overflow']:
                limit=min((bound if overflow else 4096)//bound,(high-low)//2)
                assert not row['verified_steps'] and not row['matches'];overflows+=1;continue
            model.accept()
            if row['candidate_count']<=(bound if overflow else 4096)//2:limit=min(maximum,2*limit)
            assert row['verified_steps']==row['device_steps']==high-low
            found.extend((int(m['candidate_index'],16) if mapped else int(m['scalar'],16)-begin+1,int(m['scalar'],16),m['target']) for m in row['matches'])
        wanted={(i,k,t) for i,k,pub in zip(indices,scalars,public) for t in relations(mode,pub,canonical)}
        assert len(found)==len(wanted) and set(found)==wanted,(mode,kernel,name,len(found),len(wanted))
        assert not model.gaps and summary['complete'] and int(summary['verified_steps'],16)==count
        if overflow:assert overflows>0
        report['cases'].append(dict(mode=mode,kernel=kernel,name=name,device=device,count=count,relations=len(wanted),summary=summary))
    for mode in (('xpoint','hash160','address','ethereum','vanity') if a.batch_order=='both-ends' else ('xpoint','hash160','ethereum','vanity')):
        for kernel in ([a.kernel] if a.kernel else ('direct','stepped','glv') if a.batch_order=='both-ends' else ('direct','stepped')):
            for case in cases:exercise(mode,kernel,*case)
        file=root/f'{mode}.txt'
        base=[mode,'--backend',a.backend,'--range','1:101','--targets',file]
        for step in ('0','-1',f'{N:x}',f'{1<<256:x}','junk',''):
            r=run(base+['--stride',step],False);assert not r.stdout;report['rejections']+=1
        for bad in ('reverse','dance','random-window',''):
            assert not run(base+['--batch-order',bad],False).stdout;report['rejections']+=1
        for order in ('backward','random',''):
            r=run(base+['--order',order],False);assert not r.stdout;report['rejections']+=1
    if a.hardware:
        for device in range(len(inventory['devices'])):
            mode=('xpoint','hash160','ethereum','vanity')[device%4]
            exercise(mode,a.kernel or 'stepped','visible-device',101,101+33*7,7,False,device)
        # Explicit unit stride must retain the original coordinates and relations.
        for mode in (('xpoint','hash160','address','ethereum','vanity') if a.batch_order=='both-ends' else ('xpoint','hash160','ethereum','vanity')):
            file=root/f'{mode}.txt';pub=oracle_run(a.oracle,['pub '+f'{1:064x}'])[0]
            lines,_=targets(mode,[pub]);file.write_text('\n'.join(lines))
            base=[mode,'--backend',a.backend,'--range','1:4','--targets',file]+(['--kernel',a.kernel] if a.kernel else [])
            old=run(base);unit=run(base+['--stride','1'])
            assert 'coordinate_space' not in unit[0] and old[0]['target_digest']==unit[0]['target_digest']
            assert old[1]['matches']==unit[1]['matches'] and old[-1]['verified_steps']==unit[-1]['verified_steps']
report['passed']=True;a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
