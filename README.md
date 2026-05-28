# LiteSpeedStore

I built this project to get my hands dirty with modern C++ and to see how fast I could make a simple in-memory storage engine. It’s small, it’s fast, and it doesn't leak memory.

## What I was practicing

- **Modern C++ (17+)**: Using `std::optional`.
- **Speed & Efficiency**: Playing with move semantics and value-type `Record` storage for cache-friendly iteration.
- **Not Crashing**: Added a `std::mutex` so that if I ever use multiple threads, they won't fight over the data.
- **Lazy Profiling**: Built a custom RAII Timer that does all the boring time-tracking for me automatically.

Basically, I wanted to see how "pro" I could make a key-value store while keeping the code clean enough that I wouldn't hate myself looking at it a week later.

## Persistence Layer

The engine implements a durable, crash-safe persistence layer using a Write-Ahead Log (WAL), a snapshot file, and CRC-based integrity checks.

### `litespeed.wal` Layout

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

### Running the Test

You can run the persistence test directly or through CTest:

```bash
cmake --build cmake-build-debug -j 4
./cmake-build-debug/TestPersistence
ctest --test-dir cmake-build-debug --output-on-failure
```

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

I plan to extend this project further in the future as I experiment with more features and optimizations.
<img width="1214" height="162" alt="image" src="https://github.com/user-attachments/assets/07ed081f-525f-4ea5-90ff-4c527e48c40f" />
