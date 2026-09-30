#!/usr/bin/env python3
"""Canonical jobs and real HIP checkpoint/restart, with pinned-oracle targets."""
import argparse
import hashlib
import json
from pathlib import Path
import select
import sqlite3
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "oracle"))
from oracle_selftest import check_source, run as oracle_run
from model import N

parser=argparse.ArgumentParser()
parser.add_argument("--binary",type=Path,required=True)
parser.add_argument("--oracle",type=Path,required=True)
parser.add_argument("--report",type=Path,required=True)
parser.add_argument("--hardware","--hip",dest="hardware",action="store_true")
parser.add_argument("--backend",choices=("hip","cuda"),default="hip")
args=parser.parse_args()
binary=args.binary.resolve()
report={"oracle_commit":check_source(),"binary_sha256":hashlib.sha256(binary.read_bytes()).hexdigest(),"backend":args.backend,"hardware":args.hardware,"checks":0,"cases":[]}
with tempfile.TemporaryDirectory(prefix="keyhunt-c13-cli-") as temporary:
    root=Path(temporary);state=root/"state"
    def command(family,action,*options):
        return [str(binary),family,action,"--state-dir",str(state),*map(str,options)]
    def invoke(words,ok=True):
        result=subprocess.run(words,text=True,capture_output=True,timeout=90)
        assert (result.returncode==0)==ok,(words,result.stdout,result.stderr)
        report["checks"]+=1
        return [json.loads(line) for line in result.stdout.splitlines()] if ok else result
    def call(family,action,*options,ok=True):
        return invoke(command(family,action,*options),ok)
    project=call("state","project-create","--name","C13 integration")[0]["project"]
    table=root/"babies.khb"
    subprocess.run([str(binary),"bsgs-table","build","--m","17","--output",str(table)],capture_output=True,check=True,timeout=90)
    other_table=root/"other.khb"
    subprocess.run([str(binary),"bsgs-table","build","--m","19","--output",str(other_table)],capture_output=True,check=True,timeout=90)
    bad_table=root/"bad.khb"
    corrupt=bytearray(table.read_bytes());corrupt[-1]^=1;bad_table.write_bytes(corrupt)

    def prepare(mode,begin,end,seeds,label):
        public=dict(zip(seeds,oracle_run(args.oracle,[f"pub {k:064x}" for k in seeds])))
        values={k:(p[2:66] if mode=="xpoint" else p) for k,p in public.items()}
        targets=root/(label+".txt");targets.write_text("\n".join(values.values())+"\n")
        common=["--project",project,"--mode",mode,"--range",f"{begin:x}:{end:x}",
                "--block-width",hex(end-begin),"--targets",targets]
        if mode=="bsgs":common+=["--table",table]
        created=call("checkpoint","create",*common)[0]
        # Different line order/duplicates are the same immutable target set.
        targets.write_text("\n".join(list(reversed(list(values.values())))+[next(iter(values.values()))])+"\n")
        assert call("checkpoint","create",*common)[0]==created
        scope=["--project",project,"--job",created["job"]]
        grant=call("state","claim",*scope,"--owner","test-worker","--request",label)[0]["assignments"][0]["grant"]
        run=["--backend",args.backend,"--grant",grant,"--targets",targets]
        if mode=="bsgs":run+=["--table",table]
        expected={(k,value) for k,value in values.items() if begin<=k<end}
        if mode=="xpoint":
            expected|={(N-k,value) for k,value in values.items() if begin<=N-k<end}
        return scope,run,expected,targets

    cases=[
        ("xpoint",(1<<128)-3,(1<<128)+16,[(1<<128)-3,(1<<128)+4,(1<<128)+15,(1<<128)+16],"wide-xpoint"),
        ("bsgs",N-21,N,[N-21,N-20,N-1,1000],"order-bsgs"),
        ("xpoint",1,33,[1000],"no-match"),
    ]
    def results(scope):
        return call("checkpoint","results",*scope,"--limit","1000")[0]["results"]
    def verify(scope,expected):
        rows=results(scope)
        assert {(int(r["scalar"],16),r["target_bytes"]) for r in rows}==expected
        assert len(rows)==len(expected)
        after="0";paged=[]
        while True:
            page=call("checkpoint","results",*scope,"--after",after,"--limit","1")[0]
            if not page["results"]:break
            paged+=page["results"];after=page["next_after"]
        assert paged==rows
        assert call("state","block",*scope,"--block","0")[0]["state"]=="finished"
        call("state","check")

    for mode,begin,end,seeds,label in cases:
        scope,run,expected,targets=prepare(mode,begin,end,seeds,label)
        bad_target=root/"mismatch.txt";bad_target.write_text(("00"*32 if mode=="xpoint" else oracle_run(args.oracle,["pub "+"2".zfill(64)])[0])+"\n")
        if not args.hardware:
            error=call("checkpoint","run",*run,ok=False)
            assert "not built" in error.stderr
            assert results(scope)==[]
            continue
        mismatch=run.copy();mismatch[mismatch.index("--targets")+1]=bad_target
        assert not call("checkpoint","run",*mismatch,ok=False).stdout
        if mode=="bsgs":
            for invalid_table in (other_table,bad_table):
                wrong=run.copy();wrong[wrong.index("--table")+1]=invalid_table
                assert not call("checkpoint","run",*wrong,ok=False).stdout
            tuned=["--giant-batch","2","--target-batch","4","--candidate-capacity","1"]
        else:tuned=["--batch-size","32","--candidate-capacity","1"]
        output=call("checkpoint","run",*run,*tuned)
        summary=output[-1]
        assert summary["complete"] and summary["durability"]=="local"
        assert int(summary["computed_scalars"],16)==end-begin
        if label=="no-match":assert summary["checkpoints"]==1 and summary["match_observations"]==0
        else:assert summary["overflow_replays"]>0
        verify(scope,expected)
        again=call("checkpoint","run",*run)[-1]
        assert again["batches"]==0 and int(again["resumed_scalars"],16)==end-begin
        report["cases"].append({"case":label,"summary":summary,"finished_retry":again})

    if args.hardware:
        scope,run,expected,_=prepare("xpoint",100,4196,[100,101,102,103],"dense-prefix")
        recovered=call("checkpoint","run",*run,"--batch-size","256","--candidate-capacity","1")[-1]
        assert recovered["overflow_replays"]==1 and recovered["batches"]<40
        verify(scope,expected)
        report["cases"].append({"case":"dense-prefix","summary":recovered})
        # Both a target-subset tail and a short final tile switch the automatic
        # grouping kernel. Preserve the complete dispatch history in either case.
        inventory=json.loads(subprocess.check_output([str(binary),"devices","--backend",args.backend],text=True))
        units=inventory["devices"][0]["compute_units"]
        mixed_cases=[]
        if units>4:
            # Select geometry using visible CUs so the same test also works on
            # smaller partitions; at <=4 CUs auto grouping cannot select group 1.
            subset_giants=1024*((units+123)//124)
            final_targets=min(32,(units-1)//4)
            final_giants=1024*((units+4*final_targets-1)//(4*final_targets))
            mixed_cases=[("mixed-subsets",17*subset_giants*2-1,32,31,subset_giants),
                         ("mixed-final-tile",17*(final_giants+1)-1,final_targets,final_targets,final_giants)]
        for label,width,target_count,target_batch,giants in mixed_cases:
            scope,run,expected,_=prepare("bsgs",1000,1000+width,list(range(1,target_count+1)),label)
            mixed=call("checkpoint","run",*run,"--giant-batch",str(giants),"--target-batch",str(target_batch))[-1]
            groups=mixed["bsgs_groups"]
            assert mixed["metrics_version"]==2 and [g["group_size"] for g in groups]==[1,8]
            assert mixed["bsgs_group_size"]==1 and sum(g["batches"] for g in groups)==mixed["batches"]
            assert sum(int(g["device_steps"],16) for g in groups)==int(mixed["device_steps"],16)
            assert sum(int(g["verified_device_steps"],16) for g in groups)==int(mixed["verified_device_steps"],16)
            verify(scope,expected)
            report["cases"].append({"case":label,"summary":mixed})
        for mode in ("xpoint","bsgs"):
            # The first acknowledgment is durable; kill with many batches still
            # pending, then change launch geometry and replay the exact complement.
            begin,end=1,1048577
            scope,run,expected,_=prepare(mode,begin,end,[1,2,end-1],"killed-"+mode)
            slow=["--batch-size","32"] if mode=="xpoint" else ["--giant-batch","1","--target-batch","1"]
            process=subprocess.Popen(command("checkpoint","run",*run,*slow),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            try:
                assert select.select([process.stdout],[],[],30)[0],"no checkpoint acknowledgment"
                notice=json.loads(process.stdout.readline())
                assert notice["type"]=="checkpoint" and notice["durable_results"] and notice["durability"]=="local"
                assert notice["durable_coverage"]==bool(notice["intervals"])
                # A second local executor cannot start while the first owns the journal.
                conflict=call("checkpoint","run",*run,ok=False)
                assert "already held" in conflict.stderr,conflict.stderr
                process.kill()
                process.communicate(timeout=30)
            finally:
                if process.poll() is None:process.kill();process.communicate(timeout=30)
            call("state","check")
            assert results(scope),"acknowledged match disappeared after kill"
            prior=call("state","block",*scope,"--block","0")[0]
            assert prior["state"]=="in_progress"
            fast=["--batch-size","65536"] if mode=="xpoint" else ["--giant-batch","65536","--target-batch","3","--group-size","8"]
            resumed=call("checkpoint","run",*run,*fast)[-1]
            assert int(resumed["resumed_scalars"],16)+int(resumed["computed_scalars"],16)==end-begin
            verify(scope,expected)
            report["cases"].append({"case":"killed-"+mode,"first_acknowledgment":notice,"summary":resumed})
        # A broken output stream happens after COMMIT; retry must preserve results.
        scope,run,expected,_=prepare("xpoint",1,17,[1,16],"lost-stdout")
        with open("/dev/full","w") as output:
            failed=subprocess.run(command("checkpoint","run",*run,"--batch-size","4"),
                                  stdout=output,stderr=subprocess.PIPE,text=True,timeout=90)
        assert failed.returncode==2 and "committed state is retained" in failed.stderr
        assert results(scope)
        call("checkpoint","run",*run)
        verify(scope,expected)

    for words in [
        command("checkpoint","unknown"),
        command("checkpoint","create","--project",project,"--mode","xpoint","--range","0:2","--block-width","1","--targets",root/"none"),
        command("checkpoint","results","--project",project,"--job","00"*32,"--limit","0"),
        command("checkpoint","run","--backend","cpu"),
        command("checkpoint","results","--unknown","yes"),
        command("checkpoint","results","--limit","1","--limit","2"),
    ]:invoke(words,False)

    # All C++ connections are closed before the independent SQLite reader starts.
    with sqlite3.connect(state/"progress.sqlite") as db:
        assert db.execute("PRAGMA user_version").fetchone()[0]==6
        assert db.execute("PRAGMA foreign_key_check").fetchall()==[]
        for payload,digest in db.execute("SELECT c.payload,r.payload FROM checkpoints c JOIN requests r USING(project,job,owner,operation,request)"):
            assert hashlib.sha256(payload).digest()==digest
    db.close()
report["passed"]=True
args.report.write_text(json.dumps(report,indent=2)+"\n")
print(json.dumps(report))
