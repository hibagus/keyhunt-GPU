"""Strict acceptance and units for the opt-in C16 benchmark (no GPU dependency)."""
import math
import statistics


class InvalidSample(ValueError):
    """A timing without proven work must never enter a throughput comparison."""


def require(condition, message):
    if not condition:
        raise InvalidSample(message)


def hexint(value):
    require(isinstance(value, str), "expected a hexadecimal string")
    return int(value, 16)


def distribution(values):
    require(bool(values) and all(math.isfinite(v) and v >= 0 for v in values),
            "invalid timing distribution")
    median = statistics.median(values)
    return {"count": len(values), "median": median, "min": min(values), "max": max(values),
            "spread": max(values) - min(values),
            "mad": statistics.median(abs(v - median) for v in values)}


def work(case):
    width = hexint(case["end_exclusive"]) - hexint(case["begin"])
    # Tiles have a whole number of giants except for the final scalar tail.
    steps = width if case["mode"] == "xpoint" else ((width + case["m"] - 1) // case["m"]) * case["target_count"]
    return width, steps


def match_pairs(rows, field):
    pairs = [(hexint(row["scalar"]), row[field]) for row in rows]
    require(len(pairs) == len(set(pairs)), "duplicate match observation")
    return set(pairs)


def validate_volatile(rows, case, uuid):
    require(len(rows) >= 3 and rows[0]["type"] == "start" and rows[-1]["type"] == "summary",
            "missing start/completion record")
    start, summary = rows[0], rows[-1]
    require(start["uuid"] == uuid and start["mode"] == case["mode"] and
            start["target_count"] == case["target_count"] and
            start["target_digest"] == case["target_digest"] and
            hexint(start["begin"]) == hexint(case["begin"]) and
            hexint(start["end_exclusive"]) == hexint(case["end_exclusive"]), "input/device identity mismatch")
    if case["mode"] == "bsgs":
        require(start["m"] == case["m"] and start["table_checksum"] == case["table_checksum"], "table mismatch")
    cursor, end = hexint(case["begin"]), hexint(case["end_exclusive"])
    first, tile_end, tile_steps = 0, None, 0
    batches, matches = [], []
    for row in rows[1:-1]:
        begin, stop = hexint(row["begin"]), hexint(row["end_exclusive"])
        require(begin == cursor and cursor < stop <= end, "coverage gap, overlap, or out-of-range receipt")
        if row["type"] == "batch":
            if case["mode"] == "xpoint":
                expected = stop - begin
            else:
                if tile_end is None:
                    tile_end = stop
                require(stop == tile_end and row["first_target"] == first and
                        0 < row["target_count"] <= case["target_count"] - first, "missing/overlapping BSGS target group")
                giants = (stop - begin + case["m"] - 1) // case["m"]
                require(row["giants_per_target"] == giants, "wrong giant count")
                expected = giants * row["target_count"]
            require(row["device_steps"] == expected, "attempted step count mismatch")
            require(row["verified_steps"] == (0 if row["overflow"] else expected), "overflow credited useful work")
            if row["overflow"]:
                require(not row["matches"], "overflow published matches")
            else:
                if case["mode"] == "xpoint":
                    cursor = stop
                else:
                    first += row["target_count"]
                    tile_steps += expected
                matches.extend(row["matches"])
            batches.append(row)
        elif row["type"] == "tile" and case["mode"] == "bsgs":
            require(first == case["target_count"] and row["targets_completed"] == first and
                    stop == tile_end and row["verified_target_steps"] == tile_steps, "incomplete all-target tile")
            cursor, first, tile_end, tile_steps = stop, 0, None, 0
        else:
            raise InvalidSample("unexpected search record")
    require(cursor == end and first == 0 and tile_end is None, "incomplete interval")
    expected_matches = {(hexint(k), v) for k, v in case["expected_matches"]}
    require(match_pairs(matches, "x" if case["mode"] == "xpoint" else "public_key") == expected_matches,
            "match set differs from independent oracle")
    width, steps = work(case)
    coverage_key = "verified_steps" if case["mode"] == "xpoint" else "verified_scalars"
    useful_key = "verified_steps" if case["mode"] == "xpoint" else "verified_target_steps"
    require(summary["complete"] and summary["durable_coverage"] is False and
            hexint(summary[coverage_key]) == width and hexint(summary[useful_key]) == steps and
            hexint(summary["matches"]) == len(matches), "summary coverage/match mismatch")
    require(summary["launch_count"] == len(batches) and
            summary["overflow_replays"] == sum(b["overflow"] for b in batches) and
            hexint(summary["device_steps"]) == sum(b["device_steps"] for b in batches), "summary attempt mismatch")
    metrics = {key: summary[key] for key in ("kernel_ms", "download_ms", "seed_ms", "verification_ms", "wall_ms")}
    metrics.update(preparation_ms=start["preparation_ms"], table_upload_ms=start.get("table_upload_ms", 0),
                   executor_wall_ms=sum(b["wall_ms"] for b in batches),
                   replay_kernel_ms=sum(b["kernel_ms"] for b in batches if b["overflow"]),
                   checkpoint_ms=0, revalidation_ms=0)
    for key in ("kernel_ms", "download_ms", "seed_ms", "verification_ms"):
        require(math.isclose(metrics[key], sum(b[key] for b in batches), rel_tol=1e-6, abs_tol=1e-5),
                "summary timing differs from receipts")
    metrics.update(download_bytes=sum(b["download_bytes"] for b in batches),
                   peak_device_allocation_bytes=max(b["device_allocation_bytes"] for b in batches),
                   peak_pinned_allocation_bytes=max(b["pinned_allocation_bytes"] for b in batches),
                   actual_groups=sorted({b["group_size"] for b in batches}) if case["mode"] == "bsgs" else [])
    return metrics


def validate_durable(rows, case, uuid, block, results):
    require(bool(rows) and rows[-1]["type"] == "summary", "missing durable summary")
    summary = rows[-1]
    width, steps = work(case)
    require(summary["metrics_version"] in (1, 2) and summary["uuid"] == uuid and
            summary["mode"] == case["mode"] and summary["complete"] and summary["durable_coverage"] and
            summary["durability"] == "local" and hexint(summary["resumed_scalars"]) == 0 and
            hexint(summary["computed_scalars"]) == width and hexint(summary["verified_device_steps"]) == steps,
            "durable identity/coverage mismatch")
    # A successful process alone is insufficient: reopen the journal and inspect
    # its exact union and stored results after the owner has closed it.
    require(block["state"] == "finished" and not block["remaining"] and
            [(hexint(r["begin"]), hexint(r["end_exclusive"])) for r in block["covered"]] ==
            [(hexint(case["begin"]), hexint(case["end_exclusive"]))], "journal coverage mismatch")
    expected_matches = {(hexint(k), v) for k, v in case["expected_matches"]}
    require(match_pairs(results, "target_bytes") == expected_matches and
            summary["match_observations"] == len(expected_matches), "durable oracle match mismatch")
    notices = rows[:-1]
    require(len(notices) == summary["checkpoints"] and all(r["type"] == "checkpoint" for r in notices), "checkpoint receipt count mismatch")
    require(math.isclose(sum(r["transaction_ms"] for r in notices), summary["checkpoint_ms"], rel_tol=1e-8, abs_tol=1e-6), "commit timing mismatch")
    metrics = {key: summary[key] for key in ("kernel_ms", "download_ms", "seed_ms", "verification_ms", "wall_ms",
               "preparation_ms", "executor_setup_ms", "table_upload_ms", "executor_wall_ms", "replay_kernel_ms",
               "checkpoint_ms", "revalidation_ms", "peak_device_allocation_bytes", "peak_pinned_allocation_bytes")}
    metrics["download_bytes"] = hexint(summary["download_bytes"])
    metrics["actual_groups"] = []
    if case["mode"] == "bsgs":
        metrics["last_group"] = summary["bsgs_group_size"]
        # Version 1 only recorded the final dispatch. Do not misrepresent that
        # value as a complete set when comparing frozen pre-C17 executables.
        metrics["actual_groups"] = None
        if summary["metrics_version"] == 2:
            groups = summary["bsgs_groups"]
            sizes = [g["group_size"] for g in groups]
            require(sizes == sorted(set(sizes)) and all(g in (1, 8) for g in sizes) and
                    summary["bsgs_group_size"] in sizes, "invalid BSGS group set")
            for g in groups:
                require(g["batches"] > 0 and 0 <= g["overflow_replays"] <= g["batches"] and
                        0 <= hexint(g["verified_device_steps"]) <= hexint(g["device_steps"]) and
                        math.isfinite(g["kernel_ms"]) and g["kernel_ms"] >= 0, "invalid BSGS group costs")
            for key in ("batches", "overflow_replays"):
                require(sum(g[key] for g in groups) == summary[key], "BSGS group launch totals differ")
            for key in ("device_steps", "verified_device_steps"):
                require(sum(hexint(g[key]) for g in groups) == hexint(summary[key]), "BSGS group work totals differ")
            require(math.isclose(sum(g["kernel_ms"] for g in groups), summary["kernel_ms"],
                                 rel_tol=1e-8, abs_tol=1e-6), "BSGS group kernel time differs")
            metrics["actual_groups"] = sizes
            metrics["group_costs"] = groups
    return metrics


def rates(metrics, case, process_ms, summary):
    width, steps = work(case)
    require(process_ms > 0 and math.isfinite(process_ms), "invalid process time")
    for key, value in metrics.items():
        if key.endswith("_ms"):
            require(math.isfinite(value) and value >= 0, "negative/nonfinite timing")
    require(metrics["executor_wall_ms"] > 0 and metrics["kernel_ms"] > 0, "missing execution timing")
    attempted = hexint(summary["device_steps"])
    require(attempted >= steps, "attempted work below useful work")
    return {**metrics, "process_wall_ms": process_ms, "scalar_coverage": str(width),
            "useful_device_steps": str(steps), "attempted_device_steps": str(attempted),
            "overflow_device_steps": str(attempted - steps),
            "process_scalars_per_s": width * 1000 / process_ms,
            "executor_scalars_per_s": width * 1000 / metrics["executor_wall_ms"],
            "process_useful_steps_per_s": steps * 1000 / process_ms,
            "kernel_attempted_steps_per_s": attempted * 1000 / metrics["kernel_ms"]}
