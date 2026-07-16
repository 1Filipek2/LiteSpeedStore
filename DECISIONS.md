# Decisions

Design choices the code can't express on its own, and the alternatives rejected.
Add an entry only when there was a real trade-off — this is not a changelog.

---

## 1. Field-size limits are enforced on write and on read

**Date:** 2026-07-16
**Status:** Accepted

**Invariant:** `verify()` must never return `Tampered` for a file the engine itself
wrote and nothing else modified.

`WAL::append()` accepted a field of any size; `walkChain()` rejected anything over 1 MiB
as `Tampered`. A value the engine accepted and fsynced was unreadable on reopen, and
reported as tampering. The limit was a contract with one signatory.

**Decision:**

- The limit lives in `WAL.hpp` and is enforced in `StorageEngine::set()` before the lock
  and before any write, so a rejection leaves no partial state.
- The reader bounds every length against the bytes remaining in the file, as
  `Snapshot::load()` does. An entry running past EOF is a torn tail, not tampering.
- The reader keeps its cap, but reports `Malformed` — after the write-side cap, an
  oversize field means a foreign or stale file, not tampering.
- `Tampered` is reserved for a CRC, chain-hash, or seq mismatch on a **fully present**
  entry.

**Rejected: dropping the reader's cap.** The remaining-bytes bound covers the cap's
allocation-DoS purpose but not its second one. A length can't be validated by its own
CRC — it must be trusted to know what the CRC covers. With only a remaining-bytes bound,
a flipped byte in a mid-file `value_len` looks exactly like a torn tail, and the recovery
path truncates torn tails — silently discarding every valid entry after it. The cap is
what says no legitimate writer produced this. One constant stood for two concepts; keep
both, separated.

**Consequences:** oversize values now fail at `set()`, not at recovery. Not a format
change. A new `RecoveryStatus` means `recover()` must state which statuses are safe to
adopt chain state from — the current `!= Tampered` test would let `Malformed` through.
