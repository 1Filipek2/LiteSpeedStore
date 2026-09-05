# LiteSpeedStore

A crash-safe key-value store in C++17, built as a tamper-evident journal of events.

Each key holds an ordered history of values with timestamps and durations. Every write goes
to a write-ahead log before it reaches memory, and the log is hash-chained: if anyone edits,
reorders, or deletes an entry after the fact, recovery notices and refuses to load the file.

The basic workflow is:

1. Write events. Each one is appended to the WAL and added to the in-memory history.
2. Snapshot when the log grows. State is folded to disk and the log rotates.
3. Restart (or crash). State is rebuilt from the snapshot, then the WAL is replayed.
4. Verify whenever you want. `litespeed-verify` walks the chain and reports the first bad byte.

## Features

- Hash-chained WAL. Each entry stores `SHA256(previous_hash + entry)` and a sequence number,
  so any in-place edit, reorder, or deletion is detectable at a byte offset.
- CRC32 on every entry, so a torn write from a crash can be told apart from tampering.
- Durable snapshots. `write -> fsync(file) -> rename -> fsync(dir)`, so the old snapshot is
  never damaged by a failed write and a committed one survives a power loss.
- Concurrent reads. `std::shared_mutex` lets any number of readers in; writers take the
  exclusive lock.
- Group commit. `syncEveryN` controls how often `fsync()` runs, trading durability for
  throughput.
- Portable file format. Both files carry a magic + version header, and every integer is
  written little-endian, so files are recognizable and move between architectures.
- Scope profiling. The `TRACE_SCOPE` macro records how long a block took, straight into the
  store.

## Tech stack

- **C++17** - no external runtime dependencies
- **CMake** - build system
- **Catch2** - unit tests
- **libFuzzer, ASan, UBSan, TSan** - hardening
- **clang-tidy** - enforced in CI
- **Doxygen** - API docs, published to GitHub Pages

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Sanitizer builds:

```bash
# AddressSanitizer + UBSan
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSANITIZE_ADDRESS=ON
cmake --build build-asan --parallel

# ThreadSanitizer
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DSANITIZE_THREAD=ON
cmake --build build-tsan --parallel
```

## Usage

```cpp
#include "StorageEngine.hpp"

// Opens (or creates) litespeed.wal and recovers any existing state.
StorageEngine db("litespeed.wal");

db.set("checkout", "142 ms", 142.0);
db.set("checkout", "138 ms", 138.0);

db.get("checkout");          // -> "138 ms"  (most recent value)
db.getAverage("checkout");   // -> 140.0     (mean over the history)
db.historyCount("checkout"); // -> 2

db.snapshot();               // fold state to disk, rotate the log
```

The constructor takes two more optional arguments: a per-key history cap (`0` = unlimited)
and `syncEveryN`. There is also `StorageEngine::makeInMemory()`, which skips the WAL
entirely - useful for profiling and metrics.

Timing a block of code writes the elapsed time into the store under a name:

```cpp
StorageEngine metrics = StorageEngine::makeInMemory();
{
    TRACE_SCOPE("render", metrics);
    renderFrame();
}
metrics.getAverage("render"); // average ms across every render
```

## Demo

`litespeed-demo` records a few events, verifies the chain, flips one byte, and verifies
again:

```bash
cmake --build build --target litespeed-demo litespeed-verify
./build/litespeed-demo
```

![Tamper-evidence demo](assets/demo.gif)

```text
LiteSpeedStore - tamper-evidence demo
=====================================

[agent]    recorded 3 events to /tmp/litespeed_demo.wal
[agent]    checkpoint to anchor remotely:  seq=3  head=d04cb4dfd191108f...

[verify]   chain intact - 3 entries verified  [OK]

[attacker] flipped 1 byte at offset 66 (inside event #0's recorded data)

[verify]   TAMPERING DETECTED at offset 16 (after 0 valid entries)  [FAIL]
```

The GIF is regenerated with [vhs](https://github.com/charmbracelet/vhs): `vhs assets/demo.tape`.

`litespeed-verify` runs the same check as a standalone read-only tool - it never writes to
the file:

```bash
./build/litespeed-verify litespeed.wal
```

| Exit code | Meaning |
|---|---|
| `0` | Chain intact, or a torn tail from a crash |
| `1` | Tampering detected |
| `2` | Usage or I/O error - unreadable, foreign, or stale file |

## Tests

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The suite covers SHA-256, the WAL (append, recovery, chain verification, crash injection),
and the storage engine (history, snapshots, concurrency). The demo also runs as a smoke
test - it has to detect the injected tampering or the build fails.

## Fuzzing

Both on-disk parsers have libFuzzer harnesses running under ASan + UBSan, so any crash,
over-read, or over-allocation fails the run.

```bash
cmake -S . -B build-fuzz -DCMAKE_BUILD_TYPE=Release -DFUZZ=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build-fuzz --target FuzzWAL FuzzSnapshot --parallel

./build-fuzz/FuzzWAL      fuzz/corpus          -max_total_time=60 -max_len=512
./build-fuzz/FuzzSnapshot fuzz/corpus_snapshot -max_total_time=60 -max_len=512
```

- `fuzz/fuzz_wal.cpp` feeds random bytes through `WAL::recover()`.
- `fuzz/fuzz_snapshot.cpp` wraps the input in a valid header with a matching CRC, so the
  bytes reach the length-driven part of `Snapshot::load()`.

CI runs both for 30 seconds on every push.

## File formats

All multi-byte integers are little-endian.

### litespeed.wal

A fixed 16-byte header, followed by entries:

```text
MAGIC       4 bytes    "WSL1" (0x314C5357)
VERSION     4 bytes    format version (currently 2)
FLAGS       8 bytes    reserved, currently 0
```

Each entry:

```text
CRC32            4          checksum of everything after this field
SEQ              8          monotonic sequence number - a gap means tampering
TIMESTAMP_LOW    4          epoch nanoseconds, low 32 bits
TIMESTAMP_HIGH   4          epoch nanoseconds, high 32 bits
EPOCH            8          snapshot generation this entry belongs to
KEY_LEN          4          length of the key
VALUE_LEN        4          length of the serialized value blob
TYPE             1          1 = PUT, 2 = DELETE
KEY              KEY_LEN    raw key bytes
VALUE            VALUE_LEN  duration (8 bytes) followed by the value
CHAIN_HASH       32         SHA-256 linking this entry to the one before it
```

`CHAIN_HASH` is `SHA256(previous_hash + SEQ..VALUE)`, where the previous hash is all zeros
for the first entry. Because each hash folds in the one before it, changing any entry
breaks every hash after it, and recovery can point at the exact offset where the chain
first stopped matching.

Keys and values are capped at 1 MiB, enforced both when writing and when reading. See
[DECISIONS.md](DECISIONS.md) for why the cap lives on both sides, and
[THREATMODEL.md](THREATMODEL.md) for what the hash chain does and does not protect against.

### litespeed.snap

The snapshot holds the full in-memory state, history included, so `getAverage()` and
`historyCount()` are still correct after a restart:

```text
magic + version + crc32
snapshot_epoch
key_count
  key_len + key
  record_count
    timestamp + duration + value_len + value
```

### Recovery

1. Load `litespeed.snap` if it exists and restore the full state from it.
2. Replay only the WAL entries with an epoch greater than the snapshot's, verifying the
   hash chain as it goes.
3. A partial entry at the end of the file is a torn write from a crash, so it gets
   truncated.
4. A broken hash, bad CRC, or sequence gap on a complete entry is tampering. Recovery
   throws and leaves the file untouched, so the evidence survives.

`snapshot()` writes the new snapshot durably first, then advances the WAL epoch and
truncates the log. If the truncation fails, nothing is lost: the snapshot is already valid
and the stale WAL entries are ignored by epoch anyway.

## Benchmarks

```bash
cmake --build build --target Benchmark
./build/Benchmark
```

In-memory, no WAL and no fsync (Linux x86-64, `-O3`):

| Scenario | Ops | Time | Throughput |
|---|---|---|---|
| `set()` single-thread | 500 000 | 47.4 ms | 10 540 235 ops/s |
| `get()` single-thread | 500 000 | 8.5 ms | 58 859 108 ops/s |
| `getAverage()` single-thread, 100-entry history | 500 000 | 32.1 ms | 15 571 353 ops/s |
| 4 writers + 4 readers, concurrent | 1 600 000 | 432.2 ms | 3 702 288 ops/s |

`get()` comes out 5.6x faster than `set()`, which is the shared mutex doing its job:
readers run together, writers serialize. Concurrent throughput is lower than
single-threaded here because four writers are constantly taking the exclusive lock and
shutting readers out; a read-heavy mix scales much better.

Storing history as `vector<Record>` instead of `vector<unique_ptr<Record>>` was worth
+15.6% on `getAverage()` and +57.2% on concurrent throughput, since it drops the per-record
heap allocation and keeps a history contiguous in cache.

With the WAL in front, throughput is dominated by `fsync`:

| Scenario | Ops | Time | Throughput |
|---|---|---|---|
| `syncEveryN=1` (every write durable) | 276 281 | 500 ms | 552 561 ops/s |
| `syncEveryN=16` | 291 549 | 500 ms | 583 096 ops/s |
| `syncEveryN=64` | 307 350 | 500 ms | 614 700 ops/s |

Measured on an NVMe SSD. On a spinning disk the gap is far wider, roughly 100-200 ops/s at
`syncEveryN=1`. Setting `syncEveryN=N` means the last N-1 writes can be lost on a power
cut, which is the trade you are making on purpose.

The SHA-256 chain barely shows up in these numbers. At `syncEveryN=1` the path is
fsync-bound, so tamper evidence is close to free; batching amortizes the fsync and lets
throughput climb, with hashing still only a small slice of the per-write cost.

## Project structure

```text
include/     public headers (StorageEngine, FastTrace, persistence/)
src/         implementation
tests/       Catch2 unit tests
fuzz/        libFuzzer harnesses and seed corpora
benchmark/   timing benchmark
tools/       litespeed-verify and litespeed-demo
assets/      demo GIF and its vhs tape
```

## Docs

Every public header is documented with Doxygen, and CI publishes the docs to GitHub Pages
on each push to `main`. To build them locally:

```bash
doxygen Doxyfile
open docs/html/index.html
```

`DECISIONS.md` records design choices the code cannot express on its own, along with the
alternatives that were rejected. `THREATMODEL.md` states who the tamper evidence defends
against and where its guarantees stop.

## License

[MIT](LICENSE)
