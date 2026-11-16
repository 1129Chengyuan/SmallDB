## SmallDB: Persistent Key-Value Store

SmallDB is a persistent key-value store built on a **Log-Structured Merge-Tree (LSM-Tree)** architecture, as outlined in the book "Designing Data Intensive Applications". I built this in order to understand more about the underlying mechanisms in databases and how to balance write throughput, read latency, and memory management in these storage engines.

This is a Key-Value Store.

### Architecture Overview

Essentially, LSM-Tree databases are crash-pessimistic databases used for high throughput writing. This means that the database is efficient specifically for situations where we need to continue writing to a database much more than we read from it (like writing IOT data from sensors) but also recognize that the database could crash at any time, so we have to ensure data is not lost at any point in time. 


* **Write Path**: Data is first appended to a **Write-Ahead Log (WAL)** for durability. It is then inserted into an in-memory **MemTable**, a data structure (any balanced tree or skip list works but in this case I used a red-black tree) used to keep the data in sorted order.
* **Flush**: Once the MemTable reaches a certain size threshold, it is flushed to disk as a sorted **SSTable** file.
* **Compaction:** Every so often SSTables are merged together in a process called compaction, which maintains the sorted order of SSTables and takes only the most recent value from the SSTables if the keys are identical.
* **Read Path**: Lookups check the MemTable first, then proceed through SSTables from newest to oldest. Each SSTable is kept open in a **table cache** (index, bloom filter and file descriptor in memory), so a lookup costs at most one disk read per table.
* **Delete Path:** The same as the write path, but I use a "tombstone" marker to mark the key as deleted. When merging the tombstone will force the compaction to ignore the value.
---

### File Organization

| File | Responsibility |
| --- | --- |
| `smalldb.cpp/h` | Coordinates MemTable, WAL, and SSTable management. |
| `mem_table.cpp/h` | In-memory Red-Black Tree (`std::map`) providing  access. |
| `wal.cpp/h` | Handles the append-only log for crash recovery, with a CRC32 checksum on every record. |
| `ss_table.cpp/h` | Block-based "Sorted String Tables": `sstable_builder` writes them, `ss_table` opens and reads them, `SSTableIterator` streams them for compaction. |
| `bloom_filter.cpp/h` | Probabilistic bitset used to skip SSTables that definitely don't contain a key. Stored inside each SSTable, using a stable hash. |
| `compaction.cpp/h` | Merges multiple SSTables, discarding stale keys and tombstones. |
| `slice.h` | Lightweight wrapper for pointers/lengths to avoid `std::string` copy overhead. |
| `crc32.h` | CRC32 checksum used to detect torn or corrupted WAL records. |
| `crc32c.h` | Hardware-accelerated CRC32C (ARM / x86) used to checksum every SSTable block. |
| `coding.h` | Fixed-width little-endian encoding, so SSTable files are portable across machines. |
| `file_util.h` | `fsync` helpers so flushed data actually reaches the disk. |
| `log.h` | Thread-safe logging (the client thread and the compaction thread both log). |
| `kv_engine.h` | Common interface so the same benchmark can drive SmallDB and RocksDB. |
| `tests/smalldb_test.cpp` | Crash-recovery tests, SSTable edge cases, corruption detection, and a randomized test against `std::map`. |

---
### Performance Evolution & Optimizations

The database started with several performance bottlenecks, specifically for I/O accesses. 

I implemented the following features to resolve these issues:

1. **Bloom Filters**: Initially, get() operations were O(m * n) because they required reading from multiple SSTables on disk. I implemented Bloom Filters to act as a probabilistic gatekeeper.
2. **Sparse Indexing:** To fix the slow search times caused by linear I/O scans, I added a sparse index. This allows the engine to jump to specific blocks within an SSTable rather than reading the whole file.
3. **Streaming Merge:** Originally, compaction loaded all SSTable data into RAM simultaneously, creating massive memory pressure. I refactored this into a Streaming Merge using a Min-Heap.
4. **Background Compaction:** To prevent put() operations from freezing when a flush or compaction is triggered, I moved these tasks to a background thread.
5. **Crash Safety & Correctness Fixes:** I wrote a randomized test that runs the same operations against SmallDB and a `std::map`, plus tests that simulate crashes. They exposed several bugs that could lose or resurrect data:
   * Deleted keys still in the MemTable returned their old value from disk.
   * Background compaction dropped SSTables that were flushed while it was merging.
   * The SSTable file counter was shared between threads without synchronization.
   * A crash mid-compaction could bring deleted keys back.
   * A half-written WAL record crashed recovery.

   I fixed them with a **MANIFEST** file (written atomically with write-temp + `fsync` + rename) that records which SSTables are live, **CRC32 checksums** on WAL records, and `fsync` before the WAL is cleared. All tests also pass under ThreadSanitizer.
6. **Table Cache:** Profiling showed ~80% of every `get()` was system calls (`open` 35%, `read` 35%, `lseek` 11%): each lookup reopened the SSTable, re-parsed its whole index file and closed everything again. Now each SSTable is opened once and its index, bloom filter, key range and file descriptor stay in memory. A lookup checks the key range and bloom filter, binary-searches the in-memory index, and reads the one matching chunk with a single `pread`. **Random reads went from 46 µs to 0.66 µs (~70x).** Since SSTables are immutable, the cache never needs invalidating; tables are reference-counted, so compaction can delete a file while a reader is still using it.
7. **Block-Based SSTables:** The old format had three problems:
   * every table was two files (`.sst` + `.idx`);
   * the bloom filter wasn't stored, so opening a table meant scanning all of its data to rebuild it;
   * nothing checked the data for corruption.

   Now each SSTable is one file: ~4 KB **data blocks**, then a **filter block** (the bloom filter), a **meta block** (entry count, smallest/largest key), an **index block** (the last key and location of every data block), and a fixed 64-byte **footer** at the very end that says where the other blocks are.
   * **Every block carries a CRC32C checksum**, and the footer has a **magic number**, so corruption and truncated files become errors instead of wrong answers.
   * Opening a table now reads only the footer, filter, meta and index blocks, so **opening a 1M-key database went from 63.6 ms to 0.68 ms (~93x)**.
   * A lookup reads exactly one block, and large values get a block of their own instead of being pulled in 32 records at a time.

   **Tradeoff:** verifying the checksum on every read costs ~0.4 µs, so random reads went from 0.66 µs to 1.09 µs. I chose to keep verification on every read (RocksDB's default too), because returning corrupted data silently is worse than a slightly slower read. Writing this also exposed one more data-loss bug: an unreadable compaction input was treated as an empty table and then deleted. Compaction now aborts instead.

### Internal Storage Format

#### SSTable Binary Layout (format version 2, block-based)

SSTables are immutable, sorted files. Each one is a single file:

```
[data block 0] ... [data block K-1] [filter block] [meta block] [index block] [footer: 64 bytes]
every block = [contents][crc32c checksum: 4 bytes]
```

1. **Data Blocks** (~4 KB each): records sorted by key, each one stored as
   * `key_len` (u32), `value_len` (u32), `key`, `value`.

   A record never spans two blocks, so a large value just gets one large block.
2. **Filter Block**: the serialized bloom filter for every key in the table (1% false-positive rate). It uses a stable hash (FNV-1a + a mixing step), because `std::hash` can change between compilers, and a changed hash would make a stored filter reject keys that are actually present.
3. **Meta Block**: the number of entries plus the smallest and largest key, used to skip tables whose key range can't contain the key.
4. **Index Block**: for every data block, its last key and its offset and size. A lookup binary-searches this for the first block whose last key is ≥ the key it's looking for.
5. **Footer**: a fixed 64 bytes at the end of the file, holding:
   * the locations of the filter, meta and index blocks;
   * the format version;
   * a checksum of the footer itself;
   * a magic number.

   It's written last, so a file cut short by a crash has no valid magic number at its end.

All integers are little-endian. Opening a table reads the footer, then the filter, meta and index blocks, and keeps them in memory for the table's lifetime. A lookup is one `pread` of one data block plus a checksum check.

**MANIFEST**: a small file listing the live SSTables, newest first. It's replaced atomically after every flush and compaction, and on startup any SSTable not listed in it is deleted as a leftover from a crash.

---

### Performance Benchmarks

The following benchmarks were conducted on an **Apple M3 MacBook Air**.

#### Baseline Performance (No Bloom Filters, No Sparse Index)

This version represents the initial implementation where reads were significantly bottlenecked by  I/O accesses.

| Benchmark | Operations | Duration | Throughput | Latency |
| --- | --- | --- | --- | --- |
| Sequential Writes | 5000 ops | 1085.90 ms | 4604 ops/s | 217.18 µs/op |
| Random Writes | 5000 ops | 694.05 ms | 7204 ops/s | 138.81 µs/op |
| Sequential Reads | 5000 ops | 28801.64 ms | 174 ops/s | 5760.33 µs/op |
| Random Reads | 5000 ops | 28068.32 ms | 178 ops/s | 5613.66 µs/op |
| Updates (Overwrites) | 2500 ops | 151.25 ms | 16529 ops/s | 60.50 µs/op |
| Deletions | 2500 ops | 71.24 ms | 35092 ops/s | 28.50 µs/op |
| Mixed Workload (50% R/W) | 5000 ops | 8020.51 ms | 623 ops/s | 1604.10 µs/op |
| Compaction (8 SSTables) | 1 ops | 1.43 ms | 701 ops/s | 1427.54 µs/op |
| Large Values (1024 bytes) | 500 ops | 84.00 ms | 5952 ops/s | 168.01 µs/op |
| Large Values (10240 bytes) | 500 ops | 1456.17 ms | 343 ops/s | 2912.34 µs/op |
| Large Values (102400 bytes) | 50 ops | 668.16 ms | 75 ops/s | 13363.11 µs/op |
| WAL Recovery (2500 entries) | 2500 ops | 0.08 ms | 33240703 ops/s | 0.03 µs/op |

---

#### With Bloom Filters

Implementing Bloom Filters allowed the engine to skip unnecessary SSTable reads, nearly doubling read throughput.

| Benchmark | Operations | Duration | Throughput | Latency |
| --- | --- | --- | --- | --- |
| Sequential Writes | 5000 ops | 869.82 ms | 5748 ops/s | 173.96 µs |
| Random Writes | 5000 ops | 750.38 ms | 6663 ops/s | 150.08 µs |
| Sequential Reads | 5000 ops | 15556.00 ms | 321 ops/s | 3111.20 µs |
| Random Reads | 5000 ops | 15541.42 ms | 322 ops/s | 3108.28 µs |
| Updates (Overwrites) | 2500 ops | 122.21 ms | 20456 ops/s | 48.88 µs |
| Deletions | 2500 ops | 150.13 ms | 16653 ops/s | 60.05 µs |
| Mixed Workload (50% R/W) | 5000 ops | 4634.28 ms | 1079 ops/s | 926.86 µs |
| Compaction (8 SSTables) | 1 ops | 111.99 ms | 9 ops/s | 111993.84 µs |
| Large Values (1024 bytes) | 500 ops | 159.65 ms | 3132 ops/s | 319.29 µs |
| Large Values (10240 bytes) | 500 ops | 2393.07 ms | 209 ops/s | 4786.15 µs |
| Large Values (102400 bytes) | 50 ops | 1998.90 ms | 25 ops/s | 39977.95 µs |
| WAL Recovery (2500 entries) | 2500 ops | 152.17 ms | 16429 ops/s | 60.87 µs |

---

#### With Sparse Indexing

The addition of a sparse index provided the most dramatic improvement in read latency by allowing the engine to jump to specific data blocks.

| Benchmark | Operations | Duration | Throughput | Latency |
| --- | --- | --- | --- | --- |
| Sequential Writes | 5000 ops | 393.32 ms | 12712 ops/s | 78.66 µs |
| Random Writes | 5000 ops | 430.22 ms | 11622 ops/s | 86.04 µs |
| Sequential Reads | 5000 ops | 674.92 ms | 7408 ops/s | 134.98 µs |
| Random Reads | 5000 ops | 674.08 ms | 7417 ops/s | 134.82 µs |
| Updates (Overwrites) | 2500 ops | 141.64 ms | 17650 ops/s | 56.66 µs |
| Deletions | 2500 ops | 162.60 ms | 15375 ops/s | 65.04 µs |
| Mixed Workload (50% R/W) | 5000 ops | 462.70 ms | 10806 ops/s | 92.54 µs |
| Compaction (8 SSTables) | 1 ops | 115.32 ms | 9 ops/s | 115317.23 µs |
| Large Values (1024 bytes) | 500 ops | 60.96 ms | 8202 ops/s | 121.91 µs |
| Large Values (10240 bytes) | 500 ops | 793.35 ms | 630 ops/s | 1586.71 µs |
| Large Values (102400 bytes) | 50 ops | 389.22 ms | 128 ops/s | 7784.30 µs |
| WAL Recovery (2500 entries) | 2500 ops | 28.66 ms | 87222 ops/s | 11.47 µs |

---

#### With Streaming Merge

Refactoring compaction to stream data via a min-heap reduced the memory footprint while maintaining high throughput.

| Benchmark | Operations | Duration | Throughput | Latency |
| --- | --- | --- | --- | --- |
| Sequential Writes | 5000 ops | 214.32 ms | 23329 ops/s | 42.86 µs |
| Random Writes | 5000 ops | 236.03 ms | 21184 ops/s | 47.21 µs |
| Sequential Reads | 5000 ops | 491.05 ms | 10182 ops/s | 98.21 µs |
| Random Reads | 5000 ops | 498.69 ms | 10026 ops/s | 99.74 µs |
| Updates (Overwrites) | 2500 ops | 94.46 ms | 26465 ops/s | 37.79 µs |
| Deletions | 2500 ops | 114.98 ms | 21743 ops/s | 45.99 µs |
| Mixed Workload (50% R/W) | 5000 ops | 323.23 ms | 15469 ops/s | 64.65 µs |
| Compaction (8 SSTables) | 1 ops | 84.98 ms | 12 ops/s | 84983.00 µs |
| Large Values (1024 bytes) | 500 ops | 53.32 ms | 9378 ops/s | 106.64 µs |
| Large Values (10240 bytes) | 500 ops | 755.15 ms | 662 ops/s | 1510.31 µs |
| Large Values (102400 bytes) | 50 ops | 369.92 ms | 135 ops/s | 7398.43 µs |
| WAL Recovery (2500 entries) | 2500 ops | 28.28 ms | 88387 ops/s | 11.31 µs |

---

#### With Background Compaction

Moving compaction to a background thread allowed writes to proceed without waiting for disk I/O, nearly doubling write throughput again.

| Benchmark | Operations | Duration | Throughput | Latency |
| --- | --- | --- | --- | --- |
| Sequential Writes | 5000 ops | 94.57 ms | **52874 ops/s** | 18.91 µs |
| Random Writes | 5000 ops | 94.86 ms | **52707 ops/s** | 18.97 µs |
| Sequential Reads | 5000 ops | 322.27 ms | **15515 ops/s** | 64.45 µs |
| Random Reads | 5000 ops | 328.05 ms | **15241 ops/s** | 65.61 µs |
| Updates (Overwrites) | 2500 ops | 61.31 ms | **40776 ops/s** | 24.52 µs |
| Deletions | 2500 ops | 69.41 ms | **36019 ops/s** | 27.76 µs |
| Mixed Workload (50% R/W) | 5000 ops | 221.84 ms | **22538 ops/s** | 44.37 µs |
| Compaction (8 SSTables) | 1 ops | 53.68 ms | 19 ops/s | 53677.91 µs |
| Large Values (1024 bytes) | 500 ops | 46.05 ms | 10858 ops/s | 92.09 µs |
| Large Values (10240 bytes) | 500 ops | 392.56 ms | 1274 ops/s | 785.13 µs |
| Large Values (102400 bytes) | 50 ops | 317.97 ms | 157 ops/s | 6359.49 µs |
| WAL Recovery (2500 entries) | 2500 ops | 28.84 ms | 86696 ops/s | 11.53 µs |

---

#### With Crash Safety & Correctness Fixes

The benchmark harness was also updated for this version: it now builds in Release mode by default, runs every engine through a common `kv_engine` interface, and compares against RocksDB. Because of that, **these numbers aren't directly comparable to the sections above**. They're the "before" baseline for the table cache below, measured with the same harness. Reads are still slow here (~46 µs), because every lookup reopens and re-parses the SSTable files.

| Benchmark | Operations | Duration | Throughput | Latency |
| --- | --- | --- | --- | --- |
| Sequential Writes | 5000 ops | 31.48 ms | 158822 ops/s | 6.30 µs |
| Random Writes | 5000 ops | 31.72 ms | 157627 ops/s | 6.34 µs |
| Sequential Reads | 5000 ops | 224.09 ms | 22312 ops/s | 44.82 µs |
| Random Reads | 5000 ops | 231.25 ms | 21622 ops/s | 46.25 µs |
| Updates (Overwrites) | 2500 ops | 14.61 ms | 171141 ops/s | 5.84 µs |
| Deletions | 2500 ops | 4.08 ms | 612277 ops/s | 1.63 µs |
| Mixed Workload (50% R/W) | 5000 ops | 126.33 ms | 39578 ops/s | 25.27 µs |
| Full Compaction (4766 keys) | 1 ops | 4.13 ms | 242 ops/s | 4134.75 µs |
| Large Values (1024 bytes) | 500 ops | 15.64 ms | 31975 ops/s | 31.27 µs |
| Large Values (10240 bytes) | 500 ops | 169.00 ms | 2959 ops/s | 337.99 µs |
| Large Values (102400 bytes) | 50 ops | 165.18 ms | 303 ops/s | 3303.62 µs |
| WAL Recovery (2500 entries) | 2500 ops | 1.39 ms | 1794435 ops/s | 0.56 µs |

---

#### With Table Cache

Keeping each SSTable open with its index in memory removed the file opens and index parsing from every lookup. A read is now one `pread` per candidate table plus in-memory key comparisons. **Reads got ~70x faster and the mixed workload ~7x faster**, while write performance stayed the same. Compaction is slightly slower (4.1 → 5.5 ms, though its run-to-run spread is ±2.9 ms) because opening the output table now loads its index up front, a cost paid once per table instead of on every lookup.

| Benchmark | Operations | Duration | Throughput | Latency |
| --- | --- | --- | --- | --- |
| Sequential Writes | 5000 ops | 32.44 ms | 154146 ops/s | 6.49 µs |
| Random Writes | 5000 ops | 33.00 ms | 151504 ops/s | 6.60 µs |
| Sequential Reads | 5000 ops | 3.06 ms | **1636086 ops/s** | **0.61 µs** |
| Random Reads | 5000 ops | 3.30 ms | **1517291 ops/s** | **0.66 µs** |
| Updates (Overwrites) | 2500 ops | 14.46 ms | 172923 ops/s | 5.78 µs |
| Deletions | 2500 ops | 3.80 ms | 658407 ops/s | 1.52 µs |
| Mixed Workload (50% R/W) | 5000 ops | 17.18 ms | **291068 ops/s** | **3.44 µs** |
| Full Compaction (4766 keys) | 1 ops | 5.53 ms | 181 ops/s | 5530.90 µs |
| Large Values (1024 bytes) | 500 ops | 15.86 ms | 31534 ops/s | 31.71 µs |
| Large Values (10240 bytes) | 500 ops | 177.65 ms | 2814 ops/s | 355.31 µs |
| Large Values (102400 bytes) | 50 ops | 174.77 ms | 286 ops/s | 3495.44 µs |
| WAL Recovery (2500 entries) | 2500 ops | 1.42 ms | 1763585 ops/s | 0.57 µs |

**Before vs after (same harness):**

| Benchmark | Before | After | Speedup |
| --- | --- | --- | --- |
| Random Reads | 46.25 µs | 0.66 µs | ~70x |
| Sequential Reads | 44.82 µs | 0.61 µs | ~73x |
| Mixed Workload (50% R/W) | 25.27 µs | 3.44 µs | ~7x |

---

#### With Block-Based SSTables

Each table is now one file with its bloom filter and index stored inside, and every block is checksummed. Writes got ~20% faster (smaller 4-byte length fields, and no separate index file to write and fsync). Reads got slower (0.66 → 1.09 µs) because every lookup verifies its block's CRC32C. I chose that tradeoff on purpose: see #7 above.

| Benchmark | Operations | Duration | Throughput | Latency |
| --- | --- | --- | --- | --- |
| Sequential Writes | 5000 ops | 24.33 ms | **205486 ops/s** | **4.87 µs** |
| Random Writes | 5000 ops | 26.24 ms | **190533 ops/s** | **5.25 µs** |
| Sequential Reads | 5000 ops | 5.33 ms | 937427 ops/s | 1.07 µs |
| Random Reads | 5000 ops | 5.47 ms | 913458 ops/s | 1.09 µs |
| Updates (Overwrites) | 2500 ops | 11.07 ms | 225875 ops/s | 4.43 µs |
| Deletions | 2500 ops | 3.95 ms | 633449 ops/s | 1.58 µs |
| Mixed Workload (50% R/W) | 5000 ops | 14.96 ms | 334133 ops/s | 2.99 µs |
| Full Compaction (4766 keys) | 1 ops | n/a* | n/a* | n/a* |
| Large Values (1024 bytes) | 500 ops | 14.68 ms | 34067 ops/s | 29.35 µs |
| Large Values (10240 bytes) | 500 ops | 174.84 ms | 2860 ops/s | 349.67 µs |
| Large Values (102400 bytes) | 50 ops | 152.70 ms | 327 ops/s | 3053.98 µs |
| WAL Recovery (2500 entries) | 2500 ops | 1.40 ms | 1781092 ops/s | 0.56 µs |

\* This run reported 0.00 ms. Background compaction had already merged everything before the benchmark called `compact()`, so there was nothing left to merge. Whether that happens depends on timing, so this row isn't reliable. The fix is to turn off background compaction for this one benchmark.

**Opening the database** (new benchmark: write N keys with 100-byte values, close, then time reopening; best of 5):

| Keys | Data on disk | Before (2 files, filter rebuilt) | After (block-based) | Speedup |
| --- | --- | --- | --- | --- |
| 200,000 | 26 MB | 12.89 ms | 0.22 ms | ~59x |
| 1,000,000 | 129 MB → 122 MB | 63.55 ms | 0.68 ms | ~93x |

The old open time grows with the total amount of data (rebuilding the bloom filter reads every byte), while the new one only depends on the size of the index and filter blocks. Both were measured with the files already in the OS page cache, so on a cold disk the old format would be much slower.

---

#### RocksDB Reference (same benchmark, same machine)

For context, here's RocksDB run through the same harness (same memtable size, no compression, bloom filters on, WAL not fsync'd per write, matching SmallDB). The whole dataset is ~550 KB and fits in the OS page cache, so this measures in-memory code paths, not disk performance. RocksDB is built for datasets far larger than RAM, which this benchmark doesn't test.

| Benchmark | Operations | Duration | Throughput | Latency |
| --- | --- | --- | --- | --- |
| Sequential Writes | 5000 ops | 112.20 ms | 44563 ops/s | 22.44 µs |
| Random Writes | 5000 ops | 119.08 ms | 41988 ops/s | 23.82 µs |
| Sequential Reads | 5000 ops | 7.16 ms | 698506 ops/s | 1.43 µs |
| Random Reads | 5000 ops | 7.60 ms | 657665 ops/s | 1.52 µs |
| Updates (Overwrites) | 2500 ops | 52.29 ms | 47814 ops/s | 20.91 µs |
| Deletions | 2500 ops | 15.87 ms | 157505 ops/s | 6.35 µs |
| Mixed Workload (50% R/W) | 5000 ops | 56.25 ms | 88886 ops/s | 11.25 µs |
| Full Compaction (4766 keys) | 1 ops | 21.09 ms | 47 ops/s | 21094.12 µs |
| Large Values (1024 bytes) | 500 ops | 76.73 ms | 6516 ops/s | 153.46 µs |
| Large Values (10240 bytes) | 500 ops | 1014.20 ms | 493 ops/s | 2028.41 µs |
| Large Values (102400 bytes) | 50 ops | 524.22 ms | 95 ops/s | 10484.41 µs |
| WAL Recovery (2500 entries) | 2500 ops | 27.13 ms | 92145 ops/s | 10.85 µs |

---

### Running the Code

SmallDB uses CMake for building and requires C++20. 

```bash
# Create a build directory
mkdir build && cd build

# Configure the project
cmake ..

# Build the executable
make

# Run the database benchmarks
./SmallDB

# Run the correctness tests
./smalldb_tests

```

The benchmark suite in `main.cpp` automatically generates test data, measures performance across various workloads (sequential, random, mixed), and prints a summary table to the console.

---

### Project Roadmap

#### Completed

* **Core Storage Engine**: Implementation of the `mem_table`, `wal`, and `ss_table` logic.
* **Performance Tooling**: Integrated benchmarking suite to measure latency and throughput.
* **Read Optimizations**: Implementation of `bloom_filter` and sparse indexing to resolve slow  I/O reads.
* **Memory Management**: Transition to a `streaming_merge` for compaction to handle datasets larger than RAM.
* **Concurrency**: Implementation of background compaction to prevent write operations from freezing.
* **Crash Safety**: MANIFEST of live SSTables, CRC32-checksummed WAL records, and `fsync` before the WAL is cleared.
* **Testing**: Crash-recovery tests, SSTable edge-case tests, and a randomized test against `std::map`, all passing under ThreadSanitizer.
* **Table Cache**: Each SSTable opened once with its index in memory; one `pread` per lookup (~70x faster reads).
* **Block-Based SSTables**: One file per table with data, filter, meta and index blocks, a footer, and a CRC32C checksum on every block (~93x faster database open).

#### In Progress

* **Background Flush**: `put()` still writes the SSTable itself when the MemTable fills up. Next step is to freeze the full MemTable and let the background thread flush it.

#### Not Started

* **Advanced Compaction Strategies**: Leveled Compaction to further reduce read and space amplification beyond the current size-tiered approach.
* **Storage Compression**: Implementing block-level compression (LZ4/Zstd) to reduce the on-disk footprint of SSTables. Block-based SSTables make this possible now.
* **Faster Checksums**: Splitting each block's CRC32C into three interleaved streams (~3x faster) to win back most of the ~0.4 µs per read.











**Issues**  
 

// TO BE FIXED  
put() operations may seem to freeze when the memtable has to be flushed during the write (background flush)  


// FIXED
get() is O(m*n), which is super slow since worst case is multiple I/O reads // FIXED WITH BLOOM FILTERS!!   
Searching is insanely slow due to potentially O(n) I/O accesses (sparse index)   
during compaction everything is fit into RAM, which is a gigantic memory pressure (streaming merge)  
what if the db crashes during a write to the WAL? (CRC32 checksums, recovery stops at the first bad record)  
deleted keys came back from disk while the tombstone was still in the memtable (memtable find())  
background compaction lost sstables that were flushed during the merge (only replace the inputs)  
a crash mid-compaction could resurrect deleted keys (MANIFEST)  
every get() reopened the sstable and re-read its whole index file (table cache)  
opening the db scanned every sstable to rebuild bloom filters (block-based sstables, filter stored in the file)  
no checksums on sstable data, so corruption was returned as real data (crc32c on every block)  
an unreadable compaction input was treated as empty and then deleted (compaction aborts now)  


**Completed**  
mem_table  
slice  
wal  
ss_table  
compaction  
smalldb  
bloom_filter

**In Progress**  
  
**Not Started**  
Background flush  
Leveled compaction  

**Performance (no bloom, no sparse index)**  
║ Sequential Writes                        |       5000 ops |    1085.90 ms |         4604 ops/s |     217.18 µs/op  
║ Random Writes                            |       5000 ops |     694.05 ms |         7204 ops/s |     138.81 µs/op  
║ Sequential Reads                         |       5000 ops |   28801.64 ms |          174 ops/s |    5760.33 µs/op  
║ Random Reads                             |       5000 ops |   28068.32 ms |          178 ops/s |    5613.66 µs/op  
║ Updates (Overwrites)                     |       2500 ops |     151.25 ms |        16529 ops/s |      60.50 µs/op  
║ Deletions                                |       2500 ops |      71.24 ms |        35092 ops/s |      28.50 µs/op  
║ Mixed Workload (50% R/W)                 |       5000 ops |    8020.51 ms |          623 ops/s |    1604.10 µs/op  
║ Compaction (8 SSTables)                  |          1 ops |       1.43 ms |          701 ops/s |    1427.54 µs/op  
║ Large Values (1024 bytes)                |        500 ops |      84.00 ms |         5952 ops/s |     168.01 µs/op  
║ Large Values (10240 bytes)               |        500 ops |    1456.17 ms |          343 ops/s |    2912.34 µs/op  
║ Large Values (102400 bytes)              |         50 ops |     668.16 ms |           75 ops/s |   13363.11 µs/op  
║ WAL Recovery (2500 entries)              |       2500 ops |       0.08 ms |     33240703 ops/s |       0.03 µs/op  

**With bloom filters:**  
║ Sequential Writes                   |     5000 ops |     869.82 ms |       5748 ops/s |   173.96 µs |   314.40 ms  
║ Random Writes                       |     5000 ops |     750.38 ms |       6663 ops/s |   150.08 µs |   123.09 ms  
║ Sequential Reads                    |     5000 ops |   15556.00 ms |        321 ops/s |  3111.20 µs |    62.90 ms  
║ Random Reads                        |     5000 ops |   15541.42 ms |        322 ops/s |  3108.28 µs |   137.80 ms  
║ Updates (Overwrites)                |     2500 ops |     122.21 ms |      20456 ops/s |    48.88 µs |     2.97 ms  
║ Deletions                           |     2500 ops |     150.13 ms |      16653 ops/s |    60.05 µs |     6.12 ms  
║ Mixed Workload (50% R/W)            |     5000 ops |    4634.28 ms |       1079 ops/s |   926.86 µs |   205.64 ms  
║ Compaction (8 SSTables)             |        1 ops |     111.99 ms |          9 ops/s | 111993.84 µs |    35.06 ms  
║ Large Values (1024 bytes)           |      500 ops |     159.65 ms |       3132 ops/s |   319.29 µs |    59.57 ms  
║ Large Values (10240 bytes)          |      500 ops |    2393.07 ms |        209 ops/s |  4786.15 µs |   515.21 ms  
║ Large Values (102400 bytes)         |       50 ops |    1998.90 ms |         25 ops/s | 39977.95 µs |  1091.76 ms  
║ WAL Recovery (2500 entries)         |     2500 ops |     152.17 ms |      16429 ops/s |    60.87 µs |    75.79 ms  

**With Sparse Indexes:**  
║ Sequential Writes                   |     5000 ops |     393.32 ms |      12712 ops/s |    78.66 µs |    11.46 ms
║ Random Writes                       |     5000 ops |     430.22 ms |      11622 ops/s |    86.04 µs |    10.94 ms
║ Sequential Reads                    |     5000 ops |     674.92 ms |       7408 ops/s |   134.98 µs |    19.68 ms
║ Random Reads                        |     5000 ops |     674.08 ms |       7417 ops/s |   134.82 µs |    14.02 ms
║ Updates (Overwrites)                |     2500 ops |     141.64 ms |      17650 ops/s |    56.66 µs |    16.04 ms
║ Deletions                           |     2500 ops |     162.60 ms |      15375 ops/s |    65.04 µs |     9.22 ms
║ Mixed Workload (50% R/W)            |     5000 ops |     462.70 ms |      10806 ops/s |    92.54 µs |    22.70 ms
║ Compaction (8 SSTables)             |        1 ops |     115.32 ms |          9 ops/s | 115317.23 µs |    21.07 ms
║ Large Values (1024 bytes)           |      500 ops |      60.96 ms |       8202 ops/s |   121.91 µs |     3.94 ms
║ Large Values (10240 bytes)          |      500 ops |     793.35 ms |        630 ops/s |  1586.71 µs |     7.70 ms
║ Large Values (102400 bytes)         |       50 ops |     389.22 ms |        128 ops/s |  7784.30 µs |    33.15 ms
║ WAL Recovery (2500 entries)         |     2500 ops |      28.66 ms |      87222 ops/s |    11.47 µs |     1.42 ms

**With Streaming MErge**
║ Sequential Writes                   |     5000 ops |     214.32 ms |      23329 ops/s |    42.86 µs |    11.55 ms
║ Random Writes                       |     5000 ops |     236.03 ms |      21184 ops/s |    47.21 µs |     9.33 ms
║ Sequential Reads                    |     5000 ops |     491.05 ms |      10182 ops/s |    98.21 µs |    12.91 ms
║ Random Reads                        |     5000 ops |     498.69 ms |      10026 ops/s |    99.74 µs |    16.60 ms
║ Updates (Overwrites)                |     2500 ops |      94.46 ms |      26465 ops/s |    37.79 µs |     9.74 ms
║ Deletions                           |     2500 ops |     114.98 ms |      21743 ops/s |    45.99 µs |    26.05 ms
║ Mixed Workload (50% R/W)            |     5000 ops |     323.23 ms |      15469 ops/s |    64.65 µs |    19.07 ms
║ Compaction (8 SSTables)             |        1 ops |      84.98 ms |         12 ops/s | 84983.00 µs |    12.75 ms
║ Large Values (1024 bytes)           |      500 ops |      53.32 ms |       9378 ops/s |   106.64 µs |     1.04 ms
║ Large Values (10240 bytes)          |      500 ops |     755.15 ms |        662 ops/s |  1510.31 µs |    15.24 ms
║ Large Values (102400 bytes)         |       50 ops |     369.92 ms |        135 ops/s |  7398.43 µs |    15.84 ms
║ WAL Recovery (2500 entries)         |     2500 ops |      28.28 ms |      88387 ops/s |    11.31 µs |     0.64 ms

**With Background Compaction**
║ Sequential Writes                   |     5000 ops |      94.57 ms |      52874 ops/s |    18.91 µs |    10.15 ms
║ Random Writes                       |     5000 ops |      94.86 ms |      52707 ops/s |    18.97 µs |    17.17 ms
║ Sequential Reads                    |     5000 ops |     322.27 ms |      15515 ops/s |    64.45 µs |    10.54 ms
║ Random Reads                        |     5000 ops |     328.05 ms |      15241 ops/s |    65.61 µs |    14.75 ms
║ Updates (Overwrites)                |     2500 ops |      61.31 ms |      40776 ops/s |    24.52 µs |     1.82 ms
║ Deletions                           |     2500 ops |      69.41 ms |      36019 ops/s |    27.76 µs |    11.04 ms
║ Mixed Workload (50% R/W)            |     5000 ops |     221.84 ms |      22538 ops/s |    44.37 µs |     9.99 ms
║ Compaction (8 SSTables)             |        1 ops |      53.68 ms |         19 ops/s | 53677.91 µs |    10.76 ms
║ Large Values (1024 bytes)           |      500 ops |      46.05 ms |      10858 ops/s |    92.09 µs |    12.70 ms
║ Large Values (10240 bytes)          |      500 ops |     392.56 ms |       1274 ops/s |   785.13 µs |    16.06 ms
║ Large Values (102400 bytes)         |       50 ops |     317.97 ms |        157 ops/s |  6359.49 µs |    22.17 ms
║ WAL Recovery (2500 entries)         |     2500 ops |      28.84 ms |      86696 ops/s |    11.53 µs |     1.36 ms

**With Crash Safety Fixes (new harness, Release build)**
║ Sequential Writes                   |     5000 ops |      31.48 ms |     158822 ops/s |     6.30 µs |     2.97 ms
║ Random Writes                       |     5000 ops |      31.72 ms |     157627 ops/s |     6.34 µs |     1.12 ms
║ Sequential Reads                    |     5000 ops |     224.09 ms |      22312 ops/s |    44.82 µs |     1.04 ms
║ Random Reads                        |     5000 ops |     231.25 ms |      21622 ops/s |    46.25 µs |     5.93 ms
║ Updates (Overwrites)                |     2500 ops |      14.61 ms |     171141 ops/s |     5.84 µs |     0.16 ms
║ Deletions                           |     2500 ops |       4.08 ms |     612277 ops/s |     1.63 µs |     0.59 ms
║ Mixed Workload (50% R/W)            |     5000 ops |     126.33 ms |      39578 ops/s |    25.27 µs |     3.77 ms
║ Full Compaction (4766 keys)         |        1 ops |       4.13 ms |        242 ops/s |  4134.75 µs |     3.56 ms
║ Large Values (1024 bytes)           |      500 ops |      15.64 ms |      31975 ops/s |    31.27 µs |     0.25 ms
║ Large Values (10240 bytes)          |      500 ops |     169.00 ms |       2959 ops/s |   337.99 µs |     1.75 ms
║ Large Values (102400 bytes)         |       50 ops |     165.18 ms |        303 ops/s |  3303.62 µs |     1.71 ms
║ WAL Recovery (2500 entries)         |     2500 ops |       1.39 ms |    1794435 ops/s |     0.56 µs |     0.01 ms

**With Table Cache**
║ Sequential Writes                   |     5000 ops |      32.44 ms |     154146 ops/s |     6.49 µs |     1.27 ms
║ Random Writes                       |     5000 ops |      33.00 ms |     151504 ops/s |     6.60 µs |     1.39 ms
║ Sequential Reads                    |     5000 ops |       3.06 ms |    1636086 ops/s |     0.61 µs |     0.07 ms
║ Random Reads                        |     5000 ops |       3.30 ms |    1517291 ops/s |     0.66 µs |     0.03 ms
║ Updates (Overwrites)                |     2500 ops |      14.46 ms |     172923 ops/s |     5.78 µs |     0.22 ms
║ Deletions                           |     2500 ops |       3.80 ms |     658407 ops/s |     1.52 µs |     0.32 ms
║ Mixed Workload (50% R/W)            |     5000 ops |      17.18 ms |     291068 ops/s |     3.44 µs |     0.34 ms
║ Full Compaction (4766 keys)         |        1 ops |       5.53 ms |        181 ops/s |  5530.90 µs |     2.92 ms
║ Large Values (1024 bytes)           |      500 ops |      15.86 ms |      31534 ops/s |    31.71 µs |     0.16 ms
║ Large Values (10240 bytes)          |      500 ops |     177.65 ms |       2814 ops/s |   355.31 µs |     4.36 ms
║ Large Values (102400 bytes)         |       50 ops |     174.77 ms |        286 ops/s |  3495.44 µs |    22.20 ms
║ WAL Recovery (2500 entries)         |     2500 ops |       1.42 ms |    1763585 ops/s |     0.57 µs |     0.02 ms

**With Block-Based SSTables** (Full Compaction row invalid: nothing left to compact, see above)
║ Sequential Writes                   |     5000 ops |      24.33 ms |     205486 ops/s |     4.87 µs |     0.46 ms
║ Random Writes                       |     5000 ops |      26.24 ms |     190533 ops/s |     5.25 µs |     4.06 ms
║ Sequential Reads                    |     5000 ops |       5.33 ms |     937427 ops/s |     1.07 µs |     0.20 ms
║ Random Reads                        |     5000 ops |       5.47 ms |     913458 ops/s |     1.09 µs |     0.05 ms
║ Updates (Overwrites)                |     2500 ops |      11.07 ms |     225875 ops/s |     4.43 µs |     0.19 ms
║ Deletions                           |     2500 ops |       3.95 ms |     633449 ops/s |     1.58 µs |     0.67 ms
║ Mixed Workload (50% R/W)            |     5000 ops |      14.96 ms |     334133 ops/s |     2.99 µs |     1.11 ms
║ Full Compaction (4766 keys)         |        1 ops |       0.00 ms |     710026 ops/s |     1.41 µs |     0.00 ms
║ Large Values (1024 bytes)           |      500 ops |      14.68 ms |      34067 ops/s |    29.35 µs |     0.18 ms
║ Large Values (10240 bytes)          |      500 ops |     174.84 ms |       2860 ops/s |   349.67 µs |    11.71 ms
║ Large Values (102400 bytes)         |       50 ops |     152.70 ms |        327 ops/s |  3053.98 µs |     7.49 ms
║ WAL Recovery (2500 entries)         |     2500 ops |       1.40 ms |    1781092 ops/s |     0.56 µs |     0.02 ms

DB open (best of 5): 200K keys 12.89 ms -> 0.22 ms, 1M keys 63.55 ms -> 0.68 ms
