**Issues**  
 

// TO BE FIXED  
put() operations may seem to freeze if compaction needs to happen during the write (concurrency, threading)  
what if the db crashes during a write to the WAL? (checksum, but may slow down database initialization)  


// FIXED
get() is O(m*n), which is super slow since worst case is multiple I/O reads // FIXED WITH BLOOM FILTERS!!   
Searching is insanely slow due to potentially O(n) I/O accesses (sparse index)   
during compaction everything is fit into RAM, which is a gigantic memory pressure (streaming merge)  


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
