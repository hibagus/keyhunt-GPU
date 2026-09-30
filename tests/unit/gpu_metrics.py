#!/usr/bin/env python3
"""Reject plausible-looking timing records with missing or duplicated coverage."""
import copy
from pathlib import Path
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
from gpu_metrics import InvalidSample, distribution, rates, validate_volatile, validate_durable, work


class MetricsTest(unittest.TestCase):
    def setUp(self):
        self.case = dict(mode="bsgs", begin="10000000000000000", end_exclusive="10000000000000005",
                         m=3, target_count=2, target_digest="targets", table_checksum="table", expected_matches=[])
        begin, end = self.case["begin"], self.case["end_exclusive"]
        batch = dict(type="batch", begin=begin, end_exclusive=end, first_target=0, target_count=1,
                     giants_per_target=2, group_size=8, overflow=False, device_steps=2, verified_steps=2,
                     kernel_ms=2., download_ms=1., seed_ms=.5, verification_ms=.25, wall_ms=4.,
                     download_bytes=32, device_allocation_bytes=100, pinned_allocation_bytes=20, matches=[])
        self.rows = [dict(type="start", mode="bsgs", uuid="gpu", begin=begin, end_exclusive=end,
                          m=3, target_count=2, target_digest="targets", table_checksum="table", preparation_ms=7),
                     batch, {**batch, "first_target": 1},
                     dict(type="tile", begin=begin, end_exclusive=end, targets_completed=2, verified_target_steps=4),
                     dict(type="summary", complete=True, durable_coverage=False, verified_scalars="5",
                          verified_target_steps="4", device_steps="4", matches="0", launch_count=2, overflow_replays=0,
                          kernel_ms=4., download_ms=2., seed_ms=1., verification_ms=.5, wall_ms=15.)]

    def test_tail_and_units(self):
        result = validate_volatile(self.rows, self.case, "gpu")
        values = rates(result, self.case, 20, self.rows[-1])
        self.assertEqual(work(self.case), (5, 4))
        self.assertEqual(values["process_scalars_per_s"], 250)
        self.assertEqual(values["process_useful_steps_per_s"], 200)
        self.assertEqual(result["peak_device_allocation_bytes"], 100)
        self.assertEqual(result["download_bytes"], 64)

    def test_false_completions(self):
        for row, key, value in [(2, "first_target", 0), (2, "begin", "10000000000000001"),
                                (2, "overflow", True), (3, "targets_completed", 1),
                                (4, "verified_scalars", "a"), (4, "device_steps", "5"),
                                (0, "uuid", "other"), (0, "table_checksum", "wrong")]:
            with self.subTest(key=key):
                changed = copy.deepcopy(self.rows)
                changed[row][key] = value
                with self.assertRaises(InvalidSample):
                    validate_volatile(changed, self.case, "gpu")
        with self.assertRaises(InvalidSample):
            validate_volatile(self.rows[:-1], self.case, "gpu")

    def test_overflow_cost(self):
        rows = copy.deepcopy(self.rows)
        failed = {**rows[1], "overflow": True, "verified_steps": 0}
        rows.insert(1, failed)
        rows[-1].update(device_steps="6", launch_count=3, overflow_replays=1,
                        kernel_ms=6., download_ms=3., seed_ms=1.5, verification_ms=.75)
        result = rates(validate_volatile(rows, self.case, "gpu"), self.case, 20, rows[-1])
        self.assertEqual(result["scalar_coverage"], "5")
        self.assertEqual(result["overflow_device_steps"], "2")
        self.assertEqual(result["replay_kernel_ms"], 2.)

    def test_xpoint_matches(self):
        case = {**self.case, "mode": "xpoint", "target_count": 1,
                "expected_matches": [[self.case["begin"], "point"]]}
        start, batch, summary = copy.deepcopy(self.rows[0]), copy.deepcopy(self.rows[1]), copy.deepcopy(self.rows[-1])
        start.update(mode="xpoint", target_count=1)
        batch.update(device_steps=5, verified_steps=5, matches=[dict(scalar=case["begin"], x="point")])
        summary.update(verified_steps="5", device_steps="5", matches="1", launch_count=1,
                       kernel_ms=2., download_ms=1., seed_ms=.5, verification_ms=.25)
        validate_volatile([start, batch, summary], case, "gpu")
        batch["matches"].append(batch["matches"][0])
        with self.assertRaises(InvalidSample):
            validate_volatile([start, batch, summary], case, "gpu")

    def test_durable_evidence(self):
        summary = dict(type="summary", metrics_version=1, uuid="gpu", mode="bsgs", complete=True,
                       durability="local", durable_coverage=True, resumed_scalars="0", computed_scalars="5",
                       verified_device_steps="4", device_steps="4", match_observations=0, checkpoints=1,
                       download_bytes="40", bsgs_group_size=8, peak_device_allocation_bytes=100,
                       peak_pinned_allocation_bytes=20)
        for key in ("kernel_ms", "download_ms", "seed_ms", "verification_ms", "wall_ms", "preparation_ms",
                    "executor_setup_ms", "table_upload_ms", "executor_wall_ms", "replay_kernel_ms", "checkpoint_ms", "revalidation_ms"):
            summary[key] = 1.
        rows = [dict(type="checkpoint", transaction_ms=1.), summary]
        block = dict(state="finished", remaining=[], covered=[dict(begin=self.case["begin"], end_exclusive=self.case["end_exclusive"])])
        validate_durable(rows, self.case, "gpu", block, [])
        block["covered"] = []
        with self.assertRaises(InvalidSample):
            validate_durable(rows, self.case, "gpu", block, [])

    def test_distribution(self):
        self.assertEqual(distribution([1, 3, 2, 4, 5]), dict(count=5, median=3, min=1, max=5, spread=4, mad=1))
        for values in ([], [-1], [float("nan")], [float("inf")]):
            with self.assertRaises(InvalidSample):
                distribution(values)


if __name__ == "__main__":
    unittest.main()
