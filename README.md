**Issues**  
get() is O(m*n), which is super slow since worst case is multiple I/O reads // FIXED WITH BLOOM FILTERS!!    
// TO BE FIXED  
Searching is insanely slow due to potentially O(n) I/O accesses (sparse index)    
during compaction everything is fit into RAM, which is a gigantic memory pressure (streaming merge)  
put() operations may seem to freeze if compaction needs to happen during the write (concurrency, threading)  
what if the db crashes during a write to the WAL? (checksum, but may slow down database initialization)  


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
Streaming merge  

**Performance (no bloom, no streaming merge)**  
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

