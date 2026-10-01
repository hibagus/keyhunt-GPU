#!/usr/bin/env python3
"""Independent minikey ordinal coverage, overflow and process-death recovery."""
import argparse,hashlib,json,select,subprocess,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'oracle'))
from oracle_selftest import check_source,run as oracle_run
from hash160 import hash160,address
from minikey import PUBLIC_KEYS,text,ordinal,scalar
p=argparse.ArgumentParser()
for name in ('binary','oracle','report'):p.add_argument('--'+name,type=Path,required=True)
p.add_argument('--hardware',action='store_true');p.add_argument('--backend',choices=('hip','cuda'),default='hip')
orders=p.add_mutually_exclusive_group();orders.add_argument('--reverse',action='store_true');orders.add_argument('--both-ends',action='store_true');orders.add_argument('--dance',action='store_true')
a=p.parse_args();order='dance' if a.dance else 'both-ends' if a.both_ends else 'reverse' if a.reverse else 'forward';binary=str(a.binary.resolve())
report=dict(passed=False,oracle_commit=check_source(),binary_sha256=hashlib.sha256(a.binary.read_bytes()).hexdigest(),
            hardware=a.hardware,reverse=a.reverse,ordinal_order=order,backend=a.backend,cases=[],checks=0)
with tempfile.TemporaryDirectory(prefix='kh-minikey-checkpoint-') as directory:
    root=Path(directory);state=root/'state'
    def command(family,action,*words):return [binary,family,action,'--state-dir',str(state),*map(str,words)]
    def call(family,action,*words,ok=True):
        result=subprocess.run(command(family,action,*words),capture_output=True,text=True,timeout=90)
        assert (result.returncode==0)==ok,(words,result.stdout,result.stderr)
        report['checks']+=1
        return [json.loads(line) for line in result.stdout.splitlines()] if ok else result
    def prepare(label,length,begin,end,values):
        project=call('state','project-create','--name',label)[0]['project']
        file=root/(label+'.txt');file.write_text('\n'.join(values))
        words=['--project',project,'--mode','minikeys','--length',length,'--input-format','hash160',
               '--range',f'{begin:x}:{end:x}','--block-width',f'{end-begin:x}','--targets',file]
        created=call('checkpoint','create',*words)[0]
        file.write_text('\n'.join(list(reversed(values))+[values[0]]))
        assert call('checkpoint','create',*words)[0]==created
        address_file=root/(label+'-addresses.txt');address_file.write_text('\n'.join(address(v) for v in values))
        equiv=words.copy();equiv[equiv.index('--targets')+1]=address_file;equiv[equiv.index('--input-format')+1]='address'
        assert call('checkpoint','create',*equiv)[0]==created
        assert call('checkpoint','create',*words,'--encoding','compressed')[0]['job']!=created['job']
        assert not call('checkpoint','create',*words,'--ordinal-order','reverse',ok=False).stdout
        scope=['--project',project,'--job',created['job']]
        grant=call('state','claim',*scope,'--owner','test','--request',label)[0]['assignments'][0]['grant']
        return scope,['--backend',a.backend,'--grant',grant,'--targets',file,'--length',length,'--input-format','hash160']
    def results(scope):return call('checkpoint','results',*scope,'--limit','1000')[0]['results']
    def verify(scope,expected,length):
        rows=results(scope)
        assert len(rows)==len(expected) and {(int(row['ordinal'],16),row['target_bytes']) for row in rows}==expected
        for row in rows:
            assert row['coordinate_space']=='minikey-ordinal-v1'
            assert row['minikey']==text(int(row['ordinal'],16),length)
            assert int(row['scalar'],16)==scalar(row['minikey'])
        block=call('state','block',*scope,'--block','0')[0]
        assert block['state']=='finished' and not block['remaining']
        call('state','check')
    for key in PUBLIC_KEYS:
        length=len(key);begin=ordinal(key)
        valid={o:scalar(text(o,length)) for o in range(begin,begin+4097)}
        valid={o:k for o,k in valid.items() if k is not None}
        public=oracle_run(a.oracle,[f'pub {k:064x}' for k in valid.values()])
        values={(o,tag):hash160(pub,tag) for o,pub in zip(valid,public) for tag in (1,2)}
        for label,start,count in [('overflow',begin,4097),('rejected',1,1),('last',58**(length-1),1)]:
            scope,run=prepare(f'{label}-{length}',length,start,start+count,list(values.values()))
            if not a.hardware:
                rejected=call('checkpoint','run',*run,ok=False)
                assert 'not built' in rejected.stderr and not results(scope)
                continue
            bad=root/'wrong.txt';bad.write_text('00'*20)
            mismatch=run.copy();mismatch[mismatch.index('--targets')+1]=bad
            wrong_length=run.copy();wrong_length[wrong_length.index('--length')+1]=30 if length==22 else 22
            for words in (mismatch,wrong_length,run+['--encoding','compressed'],run+['--candidate-capacity','1'],run+['--kernel','stepped']):
                assert not call('checkpoint','run',*words,ok=False).stdout
            summary=call('checkpoint','run',*run,'--ordinal-order',order,'--batch-size','8192','--candidate-capacity','2')[-1]
            assert summary['complete'] and int(summary['computed_ordinals'],16)==count and summary['durability']=='local'
            assert summary['ordinal_order']==(order)
            assert summary['coordinate_space']=='minikey-ordinal-v1' and 'computed_scalars' not in summary
            if label=='overflow':assert summary['overflow_replays']>0
            expected={(o,f'{length:02x}{tag:02x}'+v) for (o,tag),v in values.items() if start<=o<start+count}
            verify(scope,expected,length)
            again=call('checkpoint','run',*run)[-1]
            assert again['batches']==0 and int(again['resumed_ordinals'],16)==count
            report['cases'].append(dict(name=label,length=length,relations=len(expected),summary=summary))
        if not a.hardware:continue
        for first,resume in ([('dance','forward'),('forward','dance'),('dance','reverse'),('reverse','dance'),('dance','both-ends'),('both-ends','dance'),('dance','dance')] if a.dance else [('both-ends','forward'),('forward','both-ends'),('both-ends','reverse'),('reverse','both-ends'),('both-ends','both-ends')] if a.both_ends else [('reverse','forward'),('forward','reverse'),('reverse','reverse')] if a.reverse else [('forward','forward')]):
            # Kill only after an acknowledged durable prefix; all later work is
            # reconstructed from its exact ordinal complement with new batch sizes.
            start=begin-1048575 if first=='reverse' else begin
            scope,run=prepare(f'killed-{length}-{first}-{resume}',length,start,start+1048576,[values[begin,tag] for tag in (1,2)])
            process=subprocess.Popen(command('checkpoint','run',*run,'--ordinal-order',first,'--batch-size','32',*(['--checkpoint-seconds','0'] if first in ('both-ends','dance') else [])),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                assert select.select([process.stdout],[],[],30)[0],'no durable acknowledgment'
                notice=json.loads(process.stdout.readline())
                for _ in range(2 if first=='dance' else 1 if first=='both-ends' else 0):
                    # Persist every selected front before killing this owner.
                    notice=json.loads(process.stdout.readline())
                assert notice['type']=='checkpoint' and notice['durable_results'] and notice['coordinate_space']=='minikey-ordinal-v1'
                assert 'already held' in call('checkpoint','run',*run,ok=False).stderr
                process.kill();process.communicate(timeout=30)
            finally:
                if process.poll() is None:process.kill();process.communicate(timeout=30)
            call('state','check');assert results(scope)
            retained=call('state','block',*scope,'--block','0')[0]
            covered=sum(int(v['end_exclusive'],16)-int(v['begin'],16) for v in retained['covered'])
            if first in ('both-ends','dance'):assert int(retained['covered'][0]['begin'],16)==start and int(retained['covered'][-1]['end_exclusive'],16)==start+1048576
            if first=='dance':assert any(int(v['begin'],16)<=start+524288<int(v['end_exclusive'],16) for v in retained['covered'])
            summary=call('checkpoint','run',*run,'--ordinal-order',resume,'--batch-size','65536')[-1]
            assert summary['complete'] and int(summary['resumed_ordinals'],16)==covered
            assert covered+int(summary['computed_ordinals'],16)==1048576
            verify(scope,{(begin,f'{length:02x}{tag:02x}'+values[begin,tag]) for tag in (1,2)},length)
            report['cases'].append(dict(name='kill-and-restart',length=length,first=first,resume=resume,retained=retained,summary=summary))
report['passed']=True;a.report.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
