# LiteSpeedStore

A small, fast, crash-safe in-memory key-value store written in C++17. Each key holds an ordered history of values with timestamps and durations, backed by a Write-Ahead Log and periodic snapshots.

## What it demonstrates

- **Reader-writer concurrency** — `std::shared_mutex` allows unlimited concurrent reads; writes take an exclusive lock. Benchmarked to confirm the expected throughput ratio.
- **WAL persistence with CRC32** — every entry is checksummed; partial writes at the tail are detected and truncated on recovery.
- **Durable atomic snapshots** — `write → fsync(file) → rename → fsync(dir)`: the existing snapshot is never touched if a write fails, and a committed snapshot survives a power loss.
- **Portable on-disk format** — WAL and snapshot files carry a `magic + version` header and serialize all integers in explicit little-endian, so files are recognizable, versioned, and portable across architectures.
- **Group-commit** — `syncEveryN` parameter trades per-write durability for throughput; benchmarked to show the trade-off.
- **Cache-friendly storage** — history stored as `vector<Record>` (value types) rather than `vector<unique_ptr<Record>>` to avoid per-entry heap allocation and improve iteration locality.
- **RAII profiling** — `TRACE_SCOPE` macro records elapsed time directly into the store without touching production data paths.
- **Fuzz testing** — libFuzzer harness feeds random bytes into `WAL::recover()` under AddressSanitizer + UBSan; runs 30 seconds in CI on every push.
- **CI pipeline** — GitHub Actions: Debug build, AddressSanitizer, ThreadSanitizer, libFuzzer, and Doxygen → GitHub Pages.

## Building

```bash
# Standard build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel

# AddressSanitizer + UBSan
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DSANITIZE_ADDRESS=ON
cmake --build build --parallel

# ThreadSanitizer
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DSANITIZE_THREAD=ON
cmake --build build --parallel

# libFuzzer (requires clang)
cmake -B build_fuzz -DCMAKE_BUILD_TYPE=Release -DFUZZ=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build_fuzz --target FuzzWAL -j4
./build_fuzz/FuzzWAL fuzz/corpus -max_total_time=60 -max_len=512
```

## Persistence Layer

The engine implements a durable, crash-safe persistence layer using a Write-Ahead Log (WAL), a snapshot file, and CRC-based integrity checks.

### `litespeed.wal` Layout

The file starts with a fixed 16-byte header, followed by a sequence of entries.
All multi-byte integers are little-endian.

```
+----------------+----------------+------------------------------------------+
| Header field   | Size (bytes)   | Description                              |
+----------------+----------------+------------------------------------------+
| MAGIC          | 4              | "WSL1" (0x314C5357) — file identifier    |
| VERSION        | 4              | Format version (currently 1)             |
| FLAGS          | 8              | Reserved (0); future use, e.g. crypto    |
+----------------+----------------+------------------------------------------+
```

Each entry that follows the header:

```
+----------------+----------------+------------------------------------------+
| Field          | Size (bytes)   | Description                              |
+----------------+----------------+------------------------------------------+
| CRC32          | 4              | Checksum of the entire entry (after CRC) |
| TIMESTAMP_LOW  | 4              | Epoch nanoseconds (low 32 bits)          |
| TIMESTAMP_HIGH | 4              | Epoch nanoseconds (high 32 bits)         |
| EPOCH          | 8              | Snapshot generation for the entry        |
| KEY_LEN        | 4              | Length of the key string                 |
| VALUE_LEN      | 4              | Length of the serialized value blob      |
| TYPE           | 1              | 1 = PUT, 2 = DELETE                      |
| KEY            | KEY_LEN        | The raw key data                         |
| VALUE          | VALUE_LEN      | Serialized [Duration(8) + Value(N)]      |
+----------------+----------------+------------------------------------------+
```

### `litespeed.snap` Layout

The snapshot stores the full in-memory state, including history for each key, so `getAverage()` and `historyCount()` stay correct after recovery.

```text
magic + version + crc32
snapshot_epoch
key_count
  key_len + key
  record_count
    timestamp + duration + value_len + value
```

### Recovery Order

1. Load `litespeed.snap` if it exists.
2. Restore the full in-memory state from the snapshot.
3. Replay only WAL entries with an epoch greater than the snapshot epoch.
4. If the WAL was compacted, replay is small and startup is faster.

### Snapshot / Compaction

- `StorageEngine::snapshot()` writes a durable snapshot of the current in-memory state.
- After the snapshot is committed, the WAL epoch is advanced and the log is truncated for rotation.
- If the truncation step fails, the snapshot is still valid and recovery still works because old WAL entries are ignored by epoch.

### Running the Tests

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Fuzz Testing

The WAL recovery parser is hardened with a libFuzzer harness (`fuzz/fuzz_wal.cpp`). It writes random bytes to a temp file and calls `WAL::recover()` under AddressSanitizer + UBSan — any crash, heap overflow, or undefined behaviour fails the run.

```bash
cmake -B build_fuzz -DCMAKE_BUILD_TYPE=Release -DFUZZ=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build_fuzz --target FuzzWAL -j4
./build_fuzz/FuzzWAL fuzz/corpus -max_total_time=60 -max_len=512
```

CI runs this for 30 seconds on every push (see `.github/workflows/ci.yml`, job `fuzz`).

## Benchmarks

Hand-rolled timing benchmark using `StorageEngine::makeInMemory()` (no WAL, no fsync).  
Build: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target Benchmark`  
Run: `./build/Benchmark`

| Scenario | Ops | Time | Throughput |
|---|---|---|---|
| `set()` single-thread | 500 000 | 81.8 ms | **6 109 749 ops/s** |
| `get()` single-thread | 500 000 | 11.8 ms | **42 477 614 ops/s** |
| `getAverage()` single-thread (100-entry history) | 500 000 | 44.4 ms | **11 272 299 ops/s** |
| Mixed 4 writers + 4 readers (concurrent) | 1 600 000 | 380.9 ms | **4 200 534 ops/s** |

*Measured on Linux x86-64, Release build (`-O3`), in-memory store. History stored as `vector<Record>` (value types) for cache-friendly iteration.*

**Key observations:**

- `get()` is **7.0× faster** than `set()` — `shared_mutex` correctly allows concurrent reads while serialising only writes.
- Switching from `vector<unique_ptr<Record>>` to `vector<Record>` improved `getAverage()` by **+15.6%** and concurrent throughput by **+57.2%** — eliminating per-record heap allocations and improving cache locality during history iteration.
- Concurrent throughput drops vs. single-thread because 4 writers each acquire a `unique_lock`, blocking all readers. This is the expected cost of write-heavy workloads; a read-heavy workload would scale much better.

### WAL-backed: group-commit trade-off

`StorageEngine` accepts a `syncEveryN` parameter controlling how often `fsync()` is called. `syncEveryN=1` is the default — every write is durable. Higher values batch writes, trading durability for throughput.

| Scenario | Ops | Time | Throughput |
|---|---|---|---|
| WAL `set()` syncEveryN=1 (max durability) | 256 022 | 500 ms | **512 043 ops/s** |
| WAL `set()` syncEveryN=16 | 291 318 | 500 ms | **582 635 ops/s** |
| WAL `set()` syncEveryN=64 | 291 293 | 500 ms | **582 583 ops/s** |

*Measured on NVMe SSD; on HDD the gap is much larger (~100–200 ops/s at syncEveryN=1).*

`syncEveryN=1` guarantees each write survives a crash. `syncEveryN=N` risks losing the last N−1 writes on power failure — a deliberate durability trade-off, not a bug.

## API Documentation

All public headers are documented with Doxygen. CI generates and deploys the docs to GitHub Pages on every push to `main`.

To generate locally:

```bash
doxygen Doxyfile
open docs/html/index.html
```
