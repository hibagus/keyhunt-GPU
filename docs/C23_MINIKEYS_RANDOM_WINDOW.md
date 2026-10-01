# C23: exact seeded minikey random windows

This slice adds execution-only `--ordinal-order random-window` to native
minikey search, checkpoint runs and supervised workers, for 22/30-character
formats and all supported encodings. `--ordinal-seed HEX` is a 256-bit ordering
seed (default zero); `--ordinal-window 1..256` bounds tiles per window (default
64). Both overrides require random-window. Forward remains the default.

## Selection and overflow

At window creation, sample the current batch limit B. Form the next W ordinal
tiles of at most B candidates from the lowest missing endpoints, clipping at
saved gaps. A window can span several gaps, but no tile or work unit crosses a
hole. Shuffle these tiles and exhaust the window before constructing the next.
This randomizes locally within ascending windows; it is not global uniform
selection or legacy sampling with replacement. Window size one is sequential.

Within a selected tile, batches visit increasing ordinals. Overflow replans at
the same unaccepted ordinal with a smaller batch and consumes no new random
draw. Successful sub-batches consume their exact interval; the tile's remainder
is exhausted before selecting another shuffled tile. A growing batch limit is
clipped at that tile's endpoint. Receipt validation, CPU verification and
accounting precede acceptance. Later windows sample the updated batch limit.

Work units are partitioned before shuffling. Their tile count is the current
work span divided by B, rounded down with a minimum of one, capped by W and the
64-bit work-unit bound. Gap/window tails clip units. Each owner starts on its
first submitted tile and finishes on its last accepted sub-batch. Execution and
replay time belong only to that owner, excluding pauses and other owners. New
adaptive observations affect the next window. State is bounded by W tile records
and W active owners beyond the input gaps; at most W additional accepted
fragments can occur while completing a window. Huge domains are never expanded.

## Reproducible order

Descending Fisher-Yates chooses j in [0,i]. Hash `khminikey-window-v1\0`, the
32-byte big-endian seed, and the 32-byte big-endian counter using SHA-256. The
counter starts at zero and advances for every draw. Mask the first digest byte
to the smallest all-ones mask covering i and reject values above i. Counter
overflow or 1,024 rejected attempts fails closed. This specifies portable order;
the seed is not a security key or a throughput promise.

## Recovery and compatibility

Restart reconstructs the saved missing complement and resets the stream.
Seed, window, batch/work geometry, order and device may change. Sequence
reproducibility requires identical initial gaps, geometry, sizing and outcomes;
canonical ordinal coverage and deduplicated results define exact recovery.

No job/configuration/target identity, schema 7, protocol, capability or
`minikey-ordinal-v1` change. Existing minikey-capable workers may resume forward.
Start/summary and worker completion records expose `ordinal_seed` and
`ordinal_window`. Creation and non-minikey runners reject the new options.
Scalar `--order`, BSGS tile settings and coordinator block claims remain separate.

## Acceptance plan and remaining scope

Check an independent integer/hashlib model, fragmented gaps, seed endpoints,
window bounds, changing batch/work sizes, overflow retries, ownership and all
previous orders. Exercise both lengths/encodings, native device execution,
interrupted recovery, old-worker capabilities, pause/restore and supervised
HTTPS/disconnected transports on HIP and H200 CUDA. Keep source/binary identities,
executable examples and raw evidence under docs/. Scalar families' alternative
traversal remains pending; this slice does not complete all of C23.
