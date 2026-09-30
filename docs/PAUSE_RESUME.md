# Graceful pause and resume (C14)

C14 extends the verified checkpoint owner with control boundaries before each
bounded submission. It keeps C13's one owner, one assigned block and one HIP
logical device per invocation. Multi-device supervision belongs to C20.

## Owner contract

A pause stops subsequent submissions, drains the current bounded runner, verifies
its completion and forces pending exhaustive coverage into a local transaction.
Only after COMMIT does the owner report durably paused. The journal's primary
state remains `in_progress`; ownership, deadline and executor lock remain held.
Running/paused are process activity, not new allocation states.

A no-match pause commits early even if the normal ten-second timer is not due.
An overflow is never credited. BSGS subgroup matches are already committed
immediately, but an incomplete all-target tile has no scalar coverage. A live
pause retains that subgroup cursor in memory; a process restart replays the
incomplete tile and deduplicates matches.

Resume audits the saved journal and checks assignment epoch, owner, generation,
deadline and executor generation before another launch. Pause does not renew a
deadline. Expired or transferred assignments reject resume; explicit recovery is
required after the old executor stops. No network is needed for a valid local
resume.

Graceful stop uses the same forced checkpoint, releases the executor only after
cleanup, and returns a summary with `complete=false` unless the block finished.
A pause racing with the final completed batch reports completion and exits.
Errors during verification or commit do not acknowledge a durable pause.

No schema change is necessary: schema 2 already stores exact coverage, matches,
fences and immutable bindings. Activity status is only meaningful while the
exclusive process is alive. Existing supported SQLite online backup and sealed
restore APIs preserve the pause frontier; restored copies remain quarantined.

## Validation in development

`storage_checkpoint_control` exercises no-match checkpoint forcing, repeated
pause/resume, idle ownership retention, graceful stop/restart, expiry and transfer
during pause, BSGS partial-target live resume and restart, overflow replay, final
batch completion and online backup/restore while paused. Existing checkpoint
fault tests continue to exercise abrupt process exits around transaction and
acknowledgment boundaries.
