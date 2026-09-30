# C18 test environment finding

The H200 session exposes a managed `/tmp/.git` marker. The existing journal
correctly rejects any state below a Git checkout, so hardcoded `/tmp` C++
fixtures fail before exercising storage or CUDA. Python fixtures already honor
`TMPDIR`. C++ storage fixtures now use `std::filesystem::temp_directory_path()`
and therefore honor it too. Production path validation is unchanged.

Run storage/recovery tests with `TMPDIR=/var/tmp` (or another writable directory
outside all checkouts). This is test configuration, not a journal policy bypass.
The first H200 run passed CUDA field-oracle validation but rejected the affected
storage fixtures; those results are superseded by the corrected environment run.

Validation: all eight targeted CPU storage, state CLI and checkpoint-control
tests passed with `TMPDIR=/var/tmp` on this host.

## CUDA CLI test budget

The BSGS CLI corpus launches roughly 100 fresh search processes for each grouping
variant. H200 validation measured about 2.7 seconds of search preparation and
about 3.2 seconds of total process time even for short searches. The inherited
300-second parent timeout expired before the complete corpus finished. CUDA now
allows 600 seconds per BSGS corpus, retaining every case and the 90-second
per-process watchdog. CPU and HIP budgets are unchanged. This is a test-runner
budget adjustment; it does not change execution limits or search behavior.
