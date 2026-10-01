# C23: reverse BSGS tile traversal

The selected slice adds `--tile-order forward|reverse` to native BSGS, checkpoint
runs and supervised workers. Forward remains the default. Reverse visits the
highest remaining scalar interval first, then moves downwards. GPU giant/baby
reconstruction still runs forward inside each tile. This option is local to a
grant; coordinator claim policy continues to select blocks independently.

## Coverage and compatibility

BSGS receipts already certify actual scalar intervals after all canonical targets
complete. Tile order is therefore execution policy, not a new candidate mapping.
Job/configuration/table identities, schema 7, result coordinates, protocol and
capability lists stay unchanged. A restart may switch tile order, target subsets,
giant counts or device while preserving exact saved coverage and results.
`checkpoint create` does not accept the option. Non-BSGS runners reject it,
including explicit `forward`. Scalar-search `--order` remains distinct.

For a remaining interval [a,b) and width W=m*max_giants, forward selects
[a,a+min(W,b-a)); reverse selects [b-min(W,b-a),b). Products and endpoints use
checked UInt256 arithmetic. Reverse orders the saved uncovered gaps from highest
to lowest, and selects work units/tiles from each gap's upper end. Tile partitions
can change with direction or launch geometry; their verified union cannot.

Overflow certifies none of an attempt. Target-subset matches may persist before
a tile is complete; partial tiles receive no scalar coverage. Restart replays
all targets for uncovered tiles, with existing durable result deduplication.
Pause drains admitted work, preserves subgroup matches, and flushes only complete
tiles. Prepared targets/table allocations survive grant handoff.

## Acceptance gates

Independent integer tests cover exact tile bounds, high-bit and near-order
intervals, tiny exhaustive partitions and short low-end tails. Native HIP/CUDA
gates compare all target relations with pinned public-key fixtures, exercise
both BSGS grouping kernels, overflow, large origins and all visible ordinals.
Recovery gates cover fragmented complements, lost acknowledgments, killed
restarts, partial-target pause, visibility changes and direction switches.
HTTPS and disconnected-file workers must reconcile identical local/server results
and retain one prepared executor across grants. Record source/binary hashes,
findings, executable examples and raw evidence under docs/.

## Remaining C23 scope

This slice does not implement BSGS both-ends/dance traversal, new random traversal
semantics, or alternative minikey orders. Existing random block-claim policies
remain available and separate from within-block search order. Legacy random
sampling is not advertised as exhaustive. `pub2rmd` remains removed from the
main legacy executable; it is not an unimplemented active search family.
