#!/usr/bin/env python3
"""Check all supplied C23 artifact/member hashes and recompute GLV summaries."""
import argparse,hashlib,json,pathlib,subprocess,tarfile,statistics,math
p=argparse.ArgumentParser();p.add_argument('--source',type=pathlib.Path,required=True);p.add_argument('--output',type=pathlib.Path,required=True);p.add_argument('--glv',type=pathlib.Path,action='append',default=[],help='also recompute a fresh profile_glv.py report');a=p.parse_args();root=a.source;baselines=root/'docs/baselines';reports=[]
def sha(data):return hashlib.sha256(data).hexdigest()
def git(*args):return subprocess.check_output(['git','-C',str(root),*args],stderr=subprocess.PIPE)
def require(x,why):
 if not x:raise ValueError(why)
for path in sorted(baselines.glob('C23*VALIDATION.json')):
 d=json.loads(path.read_text());require(d['passed'],path.name+' not passed');entry=dict(manifest=path.name,sha256=sha(path.read_bytes()),artifacts=0,raw_members_checked=0,source_fingerprints_checked=0,source_qualifications=[])
 artifacts=d.get('artifacts',d.get('artifacts_sha256',{}));archive_paths=[]
 for name,want in artifacts.items():
  file=root/name if name.startswith('docs/') else baselines/name;data=file.read_bytes();expected=want if isinstance(want,str) else want['sha256'];require(sha(data)==expected,path.name+': '+name)
  if isinstance(want,dict) and 'bytes' in want:require(len(data)==want['bytes'],'byte length '+name)
  entry['artifacts']+=1
  if name.endswith('.tar.gz'):archive_paths.append(file)
 raw=d.get('raw_files',d.get('raw_files_sha256',{}));remaining=set(raw)
 for file in archive_paths:
  with tarfile.open(file) as t:
   members={m.name:m for m in t.getmembers() if m.isfile()}
   for name in set(members)&remaining:
    expected=raw[name] if isinstance(raw[name],str) else raw[name]['sha256'];require(sha(t.extractfile(members[name]).read())==expected,'raw member '+name);remaining.remove(name);entry['raw_members_checked']+=1
   entry['archive_regular_files']=entry.get('archive_regular_files',0)+len(members)
 require(not remaining,'missing raw members '+repr(remaining))
 # First-family manifests name a final production revision and file hashes;
 # later slices instead bind phase commits and complete artifact hashes.
 revision=d.get('production_last_changed_commit')
 if d.get('source_sha256'):
  require(revision,'source hashes lack named production revision')
  for name,expected in d['source_sha256'].items():
   try:value=git('show',revision+':'+name)
   except subprocess.CalledProcessError:value=b''
   if sha(value)!=expected:
    # Do not silently attribute a later fixture/doc fingerprint to an older
    # production revision. Resolve its historical blob and retain the caveat.
    matches=[]
    for commit in git('log','--format=%H',revision+'..HEAD','--',name).decode().splitlines():
     if sha(git('show',commit+':'+name))==expected:matches.append(commit)
    require(matches,'unresolved source fingerprint '+name)
    entry['source_qualifications'].append(dict(file=name,named_revision=revision,matching_later_revisions=matches))
   else:entry['source_fingerprints_checked']+=1
 reports.append(entry)
performance=json.loads((baselines/'C23_GLV_PERFORMANCE.json').read_text());glv={}
for path in a.glv:
 report=json.loads(path.read_text());performance['fresh-'+report['backend']]={'benchmark':report}
for backend,outer in performance.items():
 report=outer['benchmark'];require(report['passed'],'GLV benchmark not passed');data=report['measurement'];require(data['warmup_rounds']==2 and data['measured_rounds']==9,'GLV round count');rows=[]
 require(len(data['workloads'])==20,'GLV workload count')
 for work in data['workloads']:
  samples=work['samples'];require(len(samples)==27,'GLV sample count');summary=next(v for v in report['summary'] if v['family']==work['family'] and v['region']==work['region']);medians={}
  for kernel in ['direct','glv','stepped']:
   s=[v for v in samples if v['kernel']==kernel];require(sorted(v['round'] for v in s)==list(range(9)),'GLV missing rounds')
   for v in s:require(all(math.isfinite(v[k]) and v[k]>0 for k in ['kernel_ms','wall_ms']) and v['matches']==(2 if work['family'] in ['xpoint','ethereum'] else 4),'GLV timing/matches')
   medians[kernel]={metric:statistics.median(v[metric] for v in s) for metric in ['kernel_ms','wall_ms']}
   require(medians[kernel]==summary['medians'][kernel],'GLV median mismatch')
  ratios=dict(direct_over_glv=medians['direct']['kernel_ms']/medians['glv']['kernel_ms'],stepped_over_glv=medians['stepped']['kernel_ms']/medians['glv']['kernel_ms'])
  require(math.isclose(ratios['direct_over_glv'],summary['direct_over_glv']) and math.isclose(1/ratios['stepped_over_glv'],summary['glv_over_stepped']),'GLV ratio mismatch')
  rows.append(dict(family=work['family'],region=work['region'],medians=medians,**ratios))
 glv[backend]=rows
out=dict(passed=True,source=git('rev-parse','HEAD').decode().strip(),manifests=reports,glv=glv,qualification='Historical hashes establish consistency. Named production/source phase discrepancies are retained explicitly; they do not substitute for fresh builds.')
a.output.write_text(json.dumps(out,indent=2)+'\n');print(json.dumps(dict(manifests=len(reports),artifacts=sum(r['artifacts'] for r in reports),raw_members=sum(r['raw_members_checked'] for r in reports),matched_source_hashes=sum(r['source_fingerprints_checked'] for r in reports),qualifications=sum(len(r['source_qualifications']) for r in reports)),indent=2))
