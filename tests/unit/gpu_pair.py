#!/usr/bin/env python3
"""Reject unequal warm work and keep between-process pair statistics separate."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from benchmark_gpu_pair import paired_statistics, warm_samples
from gpu_metrics import InvalidSample


class PairTest(unittest.TestCase):
    def test_pair_ratios(self):
        pairs=[{'baseline':{'work':{'wall_ms':a}},'candidate':{'work':{'wall_ms':b}}}
               for a,b in [(2,1),(20,5),(1,1),(3,1),(40,8)]]
        result=paired_statistics(pairs)['work']['wall_ms']
        self.assertEqual(result['paired_baseline_over_candidate']['median'],3)
        self.assertEqual(result['paired_baseline_over_candidate']['count'],5)
        self.assertEqual(result['baseline']['median'],3)

    def test_receipts(self):
        record={'count':17,'workloads':[]}
        for name,targets,matches in [('no_match_1',1,0),('boundary_3',3,3),('no_match_32',32,0)]:
            rows=[dict(sample=i,kernel='stepped',device_steps=17,matches=matches,kernel_ms=1.,wall_ms=2.,
                       download_ms=.1,download_bytes=64,device_allocation_bytes=128) for i in range(5)]
            record['workloads'].append(dict(name=name,targets=targets,samples=rows))
        self.assertEqual(len(warm_samples(record,'xpoint',17,17,1)),3)
        direct=copy.deepcopy(record)
        for case in direct['workloads']:
            for row in case['samples']:row['kernel']='direct'
        self.assertEqual(len(warm_samples(direct,'xpoint',17,17,1,'direct')),3)
        for key,value in [('device_steps',16),('matches',1),('kernel_ms',float('nan')),('sample',-1)]:
            bad=copy.deepcopy(record);bad['workloads'][0]['samples'][0][key]=value
            with self.assertRaises(InvalidSample):warm_samples(bad,'xpoint',17,17,1)
        with self.assertRaises(InvalidSample):warm_samples(record,'xpoint',18,17,1)


if __name__=='__main__':unittest.main()
