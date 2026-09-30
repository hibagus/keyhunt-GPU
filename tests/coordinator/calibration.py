#!/usr/bin/env python3
"""Exact width recommendation, BSGS units, and mixed/unmeasured input rejection."""
import copy
import importlib.util
from pathlib import Path
spec = importlib.util.spec_from_file_location("calibrate", Path(__file__).resolve().parents[2] / "tools/calibrate_blocks.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
row = dict(queue="0", uuid="reference", cold=False, complete=True, computed_scalars=hex(101),
           device_steps=hex(22), wall_ns=3000000000, target_count=2, m=10)
run = dict(validated=True, measured=True, device_count=1, mode="bsgs", configuration="binding", targets="targets", grants=[row])
report = dict(runs=[copy.deepcopy(run) for _ in range(5)])
result = module.recommend(report, "bsgs", "0")
assert int(result["block_width"], 16) == 1454400
assert int(result["initial_work_unit_width"], 16) == 6060
assert result["effective_scalars_per_second"] == dict(numerator="101", denominator="3")
assert result["device_steps_per_second"] == dict(numerator="22", denominator="3")
for mutate in (lambda r: r["runs"].pop(), lambda r: r["runs"][0]["grants"][0].update(cold=True),
               lambda r: r["runs"][0]["grants"][0].update(uuid="different"),
               lambda r: r["runs"][0].update(validated=False),
               lambda r: r["runs"][0]["grants"][0].update(wall_ns=0)):
    bad = copy.deepcopy(report)
    mutate(bad)
    try:
        module.recommend(bad, "bsgs", "0")
    except ValueError:
        pass
    else:
        raise AssertionError("invalid calibration accepted")
print("Exact calibration, BSGS scalar/giant distinction, and unmeasured input rejection passed")
