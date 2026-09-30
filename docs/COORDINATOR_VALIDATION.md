# C15 validation and findings

## Sanitizer recovery fixture

The first focused ASAN/UBSAN run passed 16 of 17 gates. LeakSanitizer traced the
failure to the recovery test's nested JSON initializer-list wrappers when its
embedded repository request threw for an expired/fenced assignment. The disk-full
transaction assertions themselves passed; no SQLite resource leak was reported.

The fixture now completes the throwing request before constructing a successful
transport envelope. This matches the real HTTPS boundary, which returns an error
without constructing a successful response. No production error handling was
suppressed and no sanitizer exclusion was added. The recovery gate then passed,
along with the new local-launch/budget gates and twenty additional core gates.
