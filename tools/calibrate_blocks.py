#!/usr/bin/env python3
"""Recommend an immutable twelve-hour block width from validated fleet samples.

Only warmed, complete single-device samples for the explicitly selected reference
GPU qualify. This emits a recommendation; it never edits a job or worker state.
"""
import argparse
from fractions import Fraction
import json
from pathlib import Path
import statistics

ORDER = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141


def recommend(report, mode, reference):
    samples, identity = [], None
    for run in report["runs"]:
        if not run.get("validated") or not run.get("measured") or run["device_count"] != 1 or run["mode"] != mode:
            continue
        for row in run["grants"]:
            if row["queue"] != reference or row.get("cold") or not row["complete"]:
                continue
            current = (row["uuid"], row["target_count"], row["m"], run["configuration"], run["targets"])
            if identity is not None and current != identity:
                raise ValueError("reference samples mix devices or immutable search inputs")
            identity = current
            span, steps, nanoseconds = int(row["computed_scalars"], 16), int(row["device_steps"], 16), row["wall_ns"]
            if not span or not steps or not isinstance(nanoseconds, int) or nanoseconds <= 0:
                raise ValueError("invalid measured work/time")
            samples.append((Fraction(span * 1000000000, nanoseconds), Fraction(steps * 1000000000, nanoseconds)))
    if len(samples) < 5:
        raise ValueError("need at least five warmed, validated single-device samples for this reference")
    rate = statistics.median(rate for rate, _ in samples)
    device_rate = statistics.median(rate for _, rate in samples)
    alignment = identity[2] if mode == "bsgs" else 1
    if not isinstance(alignment, int) or alignment <= 0:
        raise ValueError("invalid algorithm alignment")
    raw = (rate.numerator * 43200 + rate.denominator - 1) // rate.denominator
    width = ((raw + alignment - 1) // alignment) * alignment
    work = max(alignment, (rate.numerator * 180 // rate.denominator // alignment) * alignment)
    if width >= ORDER or work >= ORDER:
        raise ValueError("recommendation exceeds scalar geometry; choose an explicit width")
    return dict(mode=mode, reference_queue=reference, reference_uuid=identity[0], samples=len(samples),
                target_count=identity[1], m=identity[2], configuration=identity[3], targets=identity[4],
                effective_scalars_per_second=dict(numerator=str(rate.numerator), denominator=str(rate.denominator)),
                device_steps_per_second=dict(numerator=str(device_rate.numerator), denominator=str(device_rate.denominator)),
                block_target_seconds=43200, work_unit_target_seconds=180, alignment=str(alignment),
                block_width=f"0x{width:064x}", initial_work_unit_width=f"0x{work:064x}",
                note="Prediction from measured complete-target wall time; pauses excluded. New jobs only.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--mode", choices=("xpoint", "bsgs"), required=True)
    parser.add_argument("--reference-device", required=True, help="explicit reference queue from the fleet report")
    args = parser.parse_args()
    print(json.dumps(recommend(json.loads(args.report.read_text()), args.mode, args.reference_device), indent=2))


if __name__ == "__main__":
    main()
