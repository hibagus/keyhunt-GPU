#!/usr/bin/env python3
"""Opt-in equivalent no-match HIP runs: volatile vs timed/per-batch checkpoints."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tests/oracle"))
from oracle_selftest import check_source,run as oracle_run

parser=argparse.ArgumentParser()
parser.add_argument("--binary",type=Path,required=True)
parser.add_argument("--oracle",type=Path,required=True)
parser.add_argument("--report",type=Path,required=True)
parser.add_argument("--repeats",type=int,default=3)
args=parser.parse_args()
if not 1<=args.repeats<=20:parser.error("repeats must be 1..20")
binary=args.binary.resolve()
def invoke(words):
    start=time.perf_counter_ns()
    result=subprocess.run([str(binary),*map(str,words)],text=True,capture_output=True,check=True,timeout=120)
    return [json.loads(line) for line in result.stdout.splitlines()],(time.perf_counter_ns()-start)/1e6
variants=("volatile","timed","every-batch")
samples={(mode,variant):[] for mode in ("xpoint","bsgs") for variant in variants}
with tempfile.TemporaryDirectory(prefix="keyhunt-c13-measure-") as temporary:
    root=Path(temporary)
    # This point and its X-only partner are both outside the measured range.
    public=oracle_run(args.oracle,["pub "+f"{1<<80:064x}"])[0]
    targets={"xpoint":root/"x.txt","bsgs":root/"b.txt"}
    targets["xpoint"].write_text(public[2:66]+"\n")
    targets["bsgs"].write_text(public+"\n")
    table=root/"babies.khb";invoke(["bsgs-table","build","--m","257","--output",table])
    for repeat in range(-1,args.repeats): # one complete warm-up round is excluded
        order=variants[repeat%3:]+variants[:repeat%3]
        for mode in ("xpoint","bsgs"):
            for variant in order:
                geometry=["--batch-size","65536"] if mode=="xpoint" else ["--giant-batch","256","--target-batch","1"]
                inputs=["--targets",targets[mode]]+([] if mode=="xpoint" else ["--table",table])
                if variant=="volatile":
                    rows,wall=invoke([mode,"--backend","hip","--range","1:100001",*inputs,*geometry])
                    summary=rows[-1];field="verified_steps" if mode=="xpoint" else "verified_scalars"
                    assert int(summary[field],16)==1048576 and int(summary["matches"],16)==0
                else:
                    state=root/f"{repeat}-{mode}-{variant}"
                    def local(family,action,*options):return invoke([family,action,"--state-dir",state,*options])[0]
                    project=local("state","project-create","--name","Synthetic C13 measurement")[0]["project"]
                    job=local("checkpoint","create","--project",project,"--mode",mode,"--range","1:100001","--block-width","100000",*inputs)[0]["job"]
                    grant=local("state","claim","--project",project,"--job",job,"--owner","measure","--request","claim")[0]["assignments"][0]["grant"]
                    rows,wall=invoke(["checkpoint","run","--state-dir",state,"--backend","hip","--grant",grant,
                                      *inputs,*geometry,"--checkpoint-seconds","10" if variant=="timed" else "0"])
                    summary=rows[-1]
                    assert summary["complete"] and int(summary["computed_scalars"],16)==1048576
                    assert summary["match_observations"]==0
                    assert 1<=summary["checkpoints"]<=16
                    if variant=="every-batch":assert summary["checkpoints"]==16
                    local("state","check")
                if repeat>=0:samples[mode,variant].append({"process_wall_ms":wall,"summary":summary})
    report={"binary_sha256":hashlib.sha256(binary.read_bytes()).hexdigest(),"oracle_commit":check_source(),
            "scope":"one HIP device, 2^20 scalars, one no-match target, one excluded warm-up round, rotating variant order",
            "timing":"fresh-process wall time includes startup, preparation, runtime, output, cleanup and checkpoint work; not kernel-only",
            "device":0,"bsgs_m":257,"repeats":args.repeats,"cases":[]}
    for (mode,variant),raw in samples.items():
        report["cases"].append({"mode":mode,"variant":variant,
            "median_process_wall_ms":statistics.median(s["process_wall_ms"] for s in raw),
            "median_checkpoint_ms":statistics.median(s["summary"].get("checkpoint_ms",0) for s in raw),"samples":raw})
args.report.write_text(json.dumps(report,indent=2)+"\n")
for case in report["cases"]:print(case["mode"],case["variant"],round(case["median_process_wall_ms"],3),round(case["median_checkpoint_ms"],3))
