# Threat Model

LiteSpeedStore is framed as a **tamper-evident, crash-safe journal of security
events** - the kind of component an endpoint agent uses to record telemetry that
must survive a crash and resist after-the-fact tampering by malware trying to
cover its tracks (anti-forensics).

This document states explicitly *who* we defend against, *what* the attacker can
do, *which guarantees* we hold, and - just as important - *where the guarantees
stop*.

## Asset

The append-only event journal (`*.wal`) and its snapshot (`*.snap`): an ordered,
timestamped record of events. The value is its **integrity and completeness** -
a forensic investigator must be able to trust that what is in the log is what
happened, in the order it happened, with nothing quietly altered or removed.

## Adversary

**On-host malware performing anti-forensics.** Its goal is to erase or alter the
record of its own activity in the local journal before that record is shipped to
a trusted server.

**Assumed capabilities:**

- Read, overwrite, truncate, or delete the journal files on disk.
- Knowledge of the on-disk format (it is documented and open).

**Out of scope** (assumptions): the adversary does not control our running
process memory, and a trusted off-host party periodically receives the
checkpoint (chain head) - see *Limits*.

## Guarantees

| Guarantee | Mechanism |
|---|---|
| **Modification of any entry is detectable** | Each entry stores `chain_hash = SHA256(prev_hash | entry)`. Changing any byte breaks that entry's hash (and CRC), and the mismatch is reported with the exact byte offset. |
| **Deletion / reordering of an interior entry is detectable** | The hash chain links every entry to its predecessor, and a monotonic `seq` field must increase by exactly one. A removed or moved entry breaks the chain and/or leaves a `seq` gap. |
| **A tampered journal is refused, not silently loaded** | `StorageEngine` throws on recovery when tampering is detected; the file is left intact so the evidence survives for inspection (`WAL::verify()` is a read-only check for a verifier tool). |
| **Crash / power loss does not lose committed events** | WAL append is `fsync`'d (configurable via `syncEveryN`); snapshots use `write -> fsync(file) -> rename -> fsync(dir)`. A torn tail write from a crash is distinguished from tampering and discarded cleanly. |

The crucial distinction the recovery logic makes: a **partial write at the very
end** (a crash interrupted the writer) is a torn tail and is truncated, while a
**fully-formed entry that fails its CRC/hash, or a `seq` gap**, is tampering and
is reported.

## Limits - tamper-*evident*, not tamper-*proof*

These are deliberate, and naming them is part of the design:

- **End-truncation needs an external anchor.** If the attacker deletes the *last*
  N entries cleanly, the remaining prefix is internally consistent - the file
  alone cannot prove entries are missing. Detecting this requires comparing the
  current chain head against a previously exported checkpoint. `WAL::head()`
  returns that checkpoint `(seq, head_hash)`; a real agent ships it to a trusted
  remote (append-only store / server), which is what makes truncation visible.

- **A fully compromised host can forge a self-consistent chain.** An attacker who
  rewrites the *entire* log can recompute every hash and produce a valid chain.
  The chain only proves integrity *relative to a head the attacker cannot
  silently change* - i.e. one anchored off-host. Locally we provide
  tamper-*evidence*, not tamper-*proofing*.

- **No confidentiality.** Entries are stored in plaintext. At-rest encryption
  (AES-GCM) would add confidentiality and is a planned optional upgrade; key
  management on an endpoint is itself a hard problem (local admin/malware may
  reach the key), so it would not change the trust limits above.

This is why production EDR products ship events off the endpoint quickly and
anchor integrity remotely. LiteSpeedStore models the **local half** of that
design: make tampering *detectable* and give the agent a checkpoint to anchor.
