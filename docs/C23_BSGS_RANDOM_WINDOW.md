# C23: exact seeded BSGS random windows

This slice adds execution-only `--tile-order random-window` to native BSGS,
checkpoint runs and supervised workers. `--tile-seed HEX` is a 256-bit seed
(default zero); `--tile-window 1..256` bounds the number of shuffled tiles
(default 64). Start/summary and worker completion records expose both settings.
The default tile order remains forward.

## Selection and bounded state

Build a window from the next W tiles at the lowest missing endpoints, clipping
every tile at saved gaps. Tile width is m times the giant-batch limit. Shuffle
those tiles, exhaust the entire window, then build the next window. Tiles may
span different missing intervals but no tile or accounting unit crosses a saved
hole. Window size one is sequential. The policy is local randomization within
ascending windows, not globally uniform random selection or legacy sampling
with replacement. Coordinator block claim policy remains independent.

At window creation, sample the current adaptive work span. Round its tile count
down, with a minimum of one tile, and partition the window into contiguous work
units of at most that many tiles. Short gap/window tails clip units. Shuffle
tiles across those units, announcing each unit at its first tile and completing
it at its last tile. New sizing observations apply to the next window. Each
unit records only its own execution time. This makes random selection meaningful
even with fixed one-tile work units and bounds simultaneous ownership by W.

Only W tile records and bounded ownership metadata are needed in addition to the
input gaps. No new window opens until every target subgroup of the previous
window's last tile is verified. At most W additional receipt fragments can be
introduced before completing the current window; huge domains are never
materialized. GPU arithmetic inside each tile stays forward.

## Reproducible shuffle

Use descending Fisher-Yates. To select j in [0,i], hash the bytes
`khbsgs-window-v1\0` followed by the 32-byte big-endian seed and 32-byte big-endian
counter using SHA-256. The counter starts at zero per invocation and advances
for every draw. Mask the first digest byte to the smallest all-ones mask covering
i; reject values greater than i. Fail closed after 1,024 rejected draws or on
counter overflow. This avoids modulo bias and library-dependent shuffle rules.
The seed is an ordering control, not a security key or a throughput guarantee.

## Recovery and compatibility

Overflow and partial target groups retain the same tile and consume no new
random draw. Verified subgroup matches may be saved early, but a tile certifies
coverage only after all targets finish. Restart rebuilds the missing complement
and resets the stream; seed, window, geometry and policy may change. The same
seed reproduces a sequence only with the same initial gaps, geometry and work
sizes. Exact scalar coverage and deduplicated results define recovery.

No job/configuration/table identity, schema 7, protocol or capability changes.
Existing BSGS workers can resume forward. Non-BSGS runners and checkpoint
creation reject the new options; seed/window overrides require random-window.
Scalar `--order` and minikey `--ordinal-order` remain separate.

## Acceptance plan and remaining scope

Check independent integer/hashlib sequences, seed endpoints, rejection draws,
window bounds, all earlier orders, fragmented recovery, changing work sizes,
subgroup overflow, lost acknowledgments and killed policy-switch restarts.
Validate both GPU kernels, every visible device, pause/restore and supervised
HTTPS/disconnected file transports on HIP and CUDA. Preserve raw evidence and
executable examples. Other families' random traversal remains pending in C23.
