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
