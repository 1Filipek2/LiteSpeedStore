# LiteSpeedStore

I built this project to get my hands dirty with modern C++ and to see how fast I could make a simple in-memory storage engine. It’s small, it’s fast, and it doesn't leak memory.

## What I was practicing

- **Modern C++ (17+)**: Using `std::optional`.
- **Speed & Efficiency**: Playing with move semantics and `std::unique_ptr` to keep things snappy and avoid useless copying.
- **Not Crashing**: Added a `std::mutex` so that if I ever use multiple threads, they won't fight over the data.
- **Lazy Profiling**: Built a custom RAII Timer that does all the boring time-tracking for me automatically.

Basically, I wanted to see how "pro" I could make a key-value store while keeping the code clean enough that I wouldn't hate myself looking at it a week later.

## Persistence Layer

The engine implements a durable, crash-safe persistence layer using a Write-Ahead Log (WAL) with integrity checks.

### `litespeed.wal` Layout

```
+----------------+----------------+------------------------------------------+
| Field          | Size (bytes)   | Description                              |
+----------------+----------------+------------------------------------------+
| CRC32          | 4              | Checksum of the entire entry (after CRC) |
| TIMESTAMP_LOW  | 4              | Epoch nanoseconds (low 32 bits)          |
| TIMESTAMP_HIGH | 4              | Epoch nanoseconds (high 32 bits)         |
| KEY_LEN        | 4              | Length of the key string                 |
| VALUE_LEN      | 4              | Length of the serialized value blob      |
| TYPE           | 1              | 1 = PUT, 2 = DELETE                      |
| KEY            | KEY_LEN        | The raw key data                         |
| VALUE          | VALUE_LEN      | Serialized [Duration(8) + Value(N)]      |
+----------------+----------------+------------------------------------------+
```

I plan to extend this project further in the future as I experiment with more features and optimizations.
