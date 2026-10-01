# Supervisor terminal-exit recovery (A21)

The supervisor now keeps a deadline after a device publishes `control:stopped`
or `exit`. These notifications precede the retained executor's destruction, so
they do not prove that the process exited. Previously `stopped` exempted a live
owner from the progress watchdog and could leave `--once` waiting indefinitely.

The first terminal event starts a fresh monotonic deadline using the configured
`--stall-seconds` value. Time spent paused does not consume this exit budget.
Repeated terminal events, progress counters or later control messages cannot
extend it. A new process resets the terminal timestamp. The existing shared
30-second drain and 5-second reap budgets, quarantine and surviving-owner locks
remain authoritative; no replacement may bypass an unreaped owner's locks.

`python3 tests/coordinator/supervisor.py` passes deterministic deadline checks
and real child-process checks. Normal terminal events exit successfully. A
synthetic child publishes terminal events and ignores termination while its peer
finishes; the supervisor kills/reaps the stuck child, preserves the peer's result
and quarantines only the faulty queue. That fixture accelerates the injected
supervisor clock and does not add a production fault switch. The existing model
also verifies that 64 unreapable children share the same total shutdown budget.

The fixture models teardown failure; it does not demonstrate recovery from a
physical driver hang. This correction is a prerequisite for extending supervised
GPU modes under C23. The frozen C21 audit and its original evidence stay unchanged.
