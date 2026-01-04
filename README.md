**Issues**  
get() is O(m*n), which is super slow since worst case is multiple I/O reads  
during compaction everything is fit into RAM, which is a gigantic memory pressure  

**Completed**  
mem_table  
slice  
wal  
ss_table  
compaction  
smalldb  

**In Progress**  
  
**Not Started**  
Bloom filters  
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
