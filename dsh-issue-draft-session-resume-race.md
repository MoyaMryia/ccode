# Race condition: resuming a live mid-turn session persists crash-recovery closers that collide with the live session's seqs → "corrupt session log: seq gap in committed region"

## Environment

- `@deepseek-ai/dsh` 0.1.0-rc.6 (npm global install)
- Node.js v24.19.0
- Linux x86_64 (kernel 6.14)

## Summary

Opening/resuming a session from a second frontend (e.g. the Web GUI session picker) **while that session is still live and mid-turn** can permanently corrupt its session log. From then on, loading the session history fails with:

```
history unavailable for session "session-...": Error: corrupt session log:
seq gap in committed region at line 7190 (expected 76053, got 76050)
```

## Root cause analysis

The guard against this exists, but only in-process. `loadLiveSnapshot` refuses to load a session with an open live turn:

```js
// @deepseek-ai/dsh-session-persistence/lib/index.js:1055
if (interruptedTurnClosers(events).length > 0)
  throw new Error(`cannot load session "${session.id}" while its live turn is open; ...`);
```

But the **cold resume path** (`prepareCore` → `commitPrepared` → `commitRepair`, same file, ~lines 988–1030) has no way to distinguish "process crashed mid-turn" from "session is live **in another process/frontend**, stream tail not yet flushed". In both cases the durable log ends with an open turn, so `interruptedTurnClosers()` synthesizes `step/end` + `turn/end{interrupted}`, the `Session` constructor appends `session/end-seed`, and `commitRepair()` **persists** them — using seq values that the still-running session has already allocated to its in-flight stream events. When the live session's async write queue flushes afterwards, two event chains with overlapping seqs exist in the committed region, and `SessionLogScanner` (correctly) refuses to interpret the log.

## Timeline reconstructed from my actual log

1. Session live in frontend A, turn 25 step 13 streaming a `bash` tool call. Durable log flushed up to seq 76049 (`step/start`); stream chunks (seq 76050+) still in the live session's write buffer.
2. Session opened from frontend B. The resume path saw an open turn at the durable tail and treated it as a crash:
   - synthetic `step/end` seq **76050**, `turn/end {"kind":"interrupted"}` seq **76051** — both reusing the previous real event's timestamp, exactly as documented in `interruptedTurnClosers` ("timestamps reuse the last real event");
   - `session/end-seed` seq **76052** (constructor marker);
   - all three persisted via `commitRepair()`.
3. The original session was still alive: the stream continued with seqs 76050–76114 (chunk rows), followed by real events 76115+ (`assistant/message`, `tool/call`, `tool/result`, further steps), flushed **after** the repair events. The log also shows `agent/inbox/spliced ... "outcome":"canceled"` around the same time — the steering message being rerouted while two instances coexisted.
4. Turn 25 later ran to completion normally (`turn/end {"kind":"completed"}` at the file tail), proving the "interrupted" closers were spurious.
5. Result: `... 76049, [step/end 76050, turn/end 76051, end-seed 76052], [chunk 76050 ...]` → seq gap at the first conflicting line.

## Reproduction (conceptual)

1. Start a session; send a prompt that triggers a long streaming step (e.g. a tool call with slow token output).
2. While it is still streaming, open/resume the same session from a second frontend or process.
3. Let the original session finish the turn.
4. Reload the session history → `corrupt session log: seq gap in committed region`.

The race window is the latency between the live session allocating seqs in memory and its write queue flushing them, which under batched persistence is easily seconds.

## Suggested fix directions

- **Cross-process liveness check before persisting repair**: a lock/heartbeat file or a durable "live owner" marker per session, so `prepareCore` can refuse (or wait) instead of repairing a session that is live elsewhere. `commitPrepared` already re-validates the stored revision (`isPreparedSourceCurrent`), but a live session that hasn't flushed yet still shows the old revision, so this check cannot catch the race.
- **Don't persist synthetic closers eagerly**: keep the repair in-memory until the resuming process actually appends a real event (taking ownership), and re-validate at that point that no new events appeared in the log.
- **Defensive alternative**: on flush, detect that the durable tail no longer matches the writer's expected cursor and rebase/abort instead of appending a conflicting chain.

## Impact & manual recovery

Affected sessions lose their entire history in the UI even though the data is intact. I recovered mine by deleting the 3 synthetic events (verified type/seq/`interrupted` reason before deletion); the remaining log is fully contiguous (seqs 0..77214) and replays cleanly through `Session.create`, including the real tool call and its result that the spurious `turn/end{interrupted}` had contradicted.

I can provide anonymized log excerpts (the bogus region and surrounding events) if helpful — I would rather not attach the full log since it contains private conversation content.
