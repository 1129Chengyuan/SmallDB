#include <iostream>
#include <chrono>
#include <random>
#include <vector>
#include <filesystem>
#include <iomanip>
#include "smalldb.h"
#include "slice.h"

namespace fs = std::filesystem;

// Benchmark utilities
class Timer {
public:
    void start() {
        start_time = std::chrono::high_resolution_clock::now();
    }

    double elapsed_ms() {
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::milli>(end - start_time).count();
    }

private:
    std::chrono::high_resolution_clock::time_point start_time;
};

class BenchmarkResult {
public:
    std::string name;
    size_t operations;
    double duration_ms;
    double ops_per_sec;
    double latency_us;

    void print() const {
        std::cout << std::setw(40) << std::left << name << " | "
                  << std::setw(10) << std::right << operations << " ops | "
                  << std::setw(10) << std::fixed << std::setprecision(2)
                  << duration_ms << " ms | "
                  << std::setw(12) << std::fixed << std::setprecision(0)
                  << ops_per_sec << " ops/s | "
                  << std::setw(10) << std::fixed << std::setprecision(2)
                  << latency_us << " µs/op" << std::endl;
    }
};

// Random data generator
class DataGenerator {
public:
    DataGenerator() : gen(std::random_device{}()), dist(0, 999999999) {}

    std::string random_key(size_t length = 16) {
        std::string key = "key_";
        key += std::to_string(dist(gen));
        while (key.length() < length) {
            key += std::to_string(dist(gen) % 10);
        }
        return key.substr(0, length);
    }

    std::string random_value(size_t length = 100) {
        std::string value;
        for (size_t i = 0; i < length; i++) {
            value += 'a' + (dist(gen) % 26);
        }
        return value;
    }

    int random_int(int max) {
        return dist(gen) % max;
    }

private:
    std::mt19937 gen;
    std::uniform_int_distribution<> dist;
};

void cleanup_bench_data() {
    if (fs::exists("./bench_data")) {
        fs::remove_all("./bench_data");
    }
    fs::create_directories("./bench_data");
}

BenchmarkResult run_benchmark(const std::string& name, size_t ops,
                              std::function<void()> fn) {
    std::cout << "Running: " << name << "..." << std::flush;
    Timer timer;
    timer.start();
    fn();
    double duration = timer.elapsed_ms();
    std::cout << " Done (" << duration << " ms)" << std::endl;

    return BenchmarkResult{
        name,
        ops,
        duration,
        (ops * 1000.0) / duration,
        (duration * 1000.0) / ops
    };
}

// ============================================================================
// Benchmark Tests
// ============================================================================

BenchmarkResult bench_sequential_writes(size_t num_ops) {
    cleanup_bench_data();
    DataGenerator gen;

    return run_benchmark("Sequential Writes", num_ops, [&]() {
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops; i++) {
            std::string key = "key_" + std::to_string(i);
            std::string value = gen.random_value(100);
            db.put(slice(key), slice(value));
        }
    });
}

BenchmarkResult bench_random_writes(size_t num_ops) {
    cleanup_bench_data();
    DataGenerator gen;

    return run_benchmark("Random Writes", num_ops, [&]() {
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops; i++) {
            std::string key = gen.random_key(16);
            std::string value = gen.random_value(100);
            db.put(slice(key), slice(value));
        }
    });
}

BenchmarkResult bench_sequential_reads(size_t num_ops) {
    cleanup_bench_data();
    DataGenerator gen;

    // Setup: Write data
    std::cout << "  [Setup] Writing " << num_ops << " entries..." << std::flush;
    {
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops; i++) {
            std::string key = "key_" + std::to_string(i);
            std::string value = gen.random_value(100);
            db.put(slice(key), slice(value));
        }
    }
    std::cout << " Done" << std::endl;

    // Benchmark: Read data (within same DB instance to avoid reopen overhead)
    std::cout << "  [Benchmark] Reading..." << std::flush;
    return run_benchmark("Sequential Reads", num_ops, [&]() {
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops; i++) {
            std::string key = "key_" + std::to_string(i);
            volatile std::string value = db.get(slice(key));
            (void)value;
        }
    });
}

BenchmarkResult bench_random_reads(size_t num_ops) {
    cleanup_bench_data();
    DataGenerator gen;

    std::vector<std::string> keys;

    // Setup: Write data
    std::cout << "  [Setup] Writing " << num_ops << " entries..." << std::flush;
    {
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops; i++) {
            std::string key = "key_" + std::to_string(i);
            keys.push_back(key);
            std::string value = gen.random_value(100);
            db.put(slice(key), slice(value));
        }
    }
    std::cout << " Done" << std::endl;

    // Benchmark: Random reads
    std::cout << "  [Benchmark] Reading..." << std::flush;
    return run_benchmark("Random Reads", num_ops, [&]() {
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops; i++) {
            size_t idx = gen.random_int(keys.size());
            volatile std::string value = db.get(slice(keys[idx]));
            (void)value;
        }
    });
}

BenchmarkResult bench_mixed_workload(size_t num_ops) {
    cleanup_bench_data();
    DataGenerator gen;

    std::vector<std::string> keys;

    // Pre-populate some keys
    std::cout << "  [Setup] Pre-populating..." << std::flush;
    {
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops / 2; i++) {
            std::string key = "key_" + std::to_string(i);
            keys.push_back(key);
            db.put(slice(key), slice(gen.random_value(100)));
        }
    }
    std::cout << " Done" << std::endl;

    // Benchmark: 50% reads, 50% writes
    std::cout << "  [Benchmark] Mixed ops..." << std::flush;
    return run_benchmark("Mixed Workload (50% R/W)", num_ops, [&]() {
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops; i++) {
            if (i % 2 == 0) {
                std::string key = "key_" + std::to_string(i);
                db.put(slice(key), slice(gen.random_value(100)));
            } else {
                if (!keys.empty()) {
                    size_t idx = gen.random_int(keys.size());
                    volatile std::string value = db.get(slice(keys[idx]));
                    (void)value;
                }
            }
        }
    });
}

BenchmarkResult bench_updates(size_t num_ops) {
    cleanup_bench_data();
    DataGenerator gen;

    // Setup: Initial writes
    std::cout << "  [Setup] Writing initial data..." << std::flush;
    {
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < 1000; i++) {
            std::string key = "key_" + std::to_string(i);
            db.put(slice(key), slice(gen.random_value(100)));
        }
    }
    std::cout << " Done" << std::endl;

    // Benchmark: Update existing keys
    std::cout << "  [Benchmark] Updating..." << std::flush;
    return run_benchmark("Updates (Overwrites)", num_ops, [&]() {
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops; i++) {
            std::string key = "key_" + std::to_string(i % 1000);
            db.put(slice(key), slice(gen.random_value(100)));
        }
    });
}

BenchmarkResult bench_deletions(size_t num_ops) {
    cleanup_bench_data();
    DataGenerator gen;

    // Setup: Write data
    std::cout << "  [Setup] Writing data..." << std::flush;
    {
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops; i++) {
            std::string key = "key_" + std::to_string(i);
            db.put(slice(key), slice(gen.random_value(100)));
        }
    }
    std::cout << " Done" << std::endl;

    // Benchmark: Deletions
    std::cout << "  [Benchmark] Deleting..." << std::flush;
    return run_benchmark("Deletions", num_ops, [&]() {
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops; i++) {
            std::string key = "key_" + std::to_string(i);
            db.remove(slice(key));
        }
    });
}

BenchmarkResult bench_compaction(size_t num_sstables) {
    cleanup_bench_data();
    DataGenerator gen;

    std::cout << "  [Setup] Creating " << num_sstables << " SSTables..." << std::flush;
    smalldb db("./bench_data", 100);

    for (size_t i = 0; i < num_sstables * 25; i++) {
        std::string key = "key_" + std::to_string(i);
        db.put(slice(key), slice(gen.random_value(100)));
    }
    std::cout << " Done" << std::endl;

    // Benchmark: Single compaction
    std::cout << "  [Benchmark] Compacting..." << std::flush;
    Timer timer;
    timer.start();
    db.compact();
    double duration = timer.elapsed_ms();
    std::cout << " Done (" << duration << " ms)" << std::endl;

    return BenchmarkResult{
        "Compaction (" + std::to_string(num_sstables) + " SSTables)",
        1,
        duration,
        1000.0 / duration,
        duration * 1000.0
    };
}

BenchmarkResult bench_large_values(size_t num_ops, size_t value_size) {
    cleanup_bench_data();
    DataGenerator gen;

    return run_benchmark("Large Values (" + std::to_string(value_size) + " bytes)",
                        num_ops, [&]() {
        smalldb db("./bench_data", 16384);
        for (size_t i = 0; i < num_ops; i++) {
            std::string key = "key_" + std::to_string(i);
            std::string value = gen.random_value(value_size);
            db.put(slice(key), slice(value));
        }
    });
}

BenchmarkResult bench_recovery(size_t num_ops) {
    cleanup_bench_data();
    DataGenerator gen;

    // Setup: Write to WAL without flushing
    std::cout << "  [Setup] Writing to WAL..." << std::flush;
    {
        smalldb db("./bench_data", 1000000);
        for (size_t i = 0; i < num_ops; i++) {
            std::string key = "key_" + std::to_string(i);
            db.put(slice(key), slice(gen.random_value(100)));
        }
    }
    std::cout << " Done" << std::endl;

    // Benchmark: Recovery from WAL
    std::cout << "  [Benchmark] Recovering..." << std::flush;
    Timer timer;
    timer.start();
    smalldb db("./bench_data", 1000000);
    double duration = timer.elapsed_ms();
    std::cout << " Done (" << duration << " ms)" << std::endl;

    return BenchmarkResult{
        "WAL Recovery (" + std::to_string(num_ops) + " entries)",
        num_ops,
        duration,
        (num_ops * 1000.0) / duration,
        (duration * 1000.0) / num_ops
    };
}

// ============================================================================
// Main Benchmark Suite
// ============================================================================

int main() {
    std::cout << "\n";
    std::cout << "╔════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    SmallDB Performance Benchmark Suite                     ║\n";
    std::cout << "║                         Apple M3 MacBook Air                               ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";

    std::vector<BenchmarkResult> results;

    // Basic operations (reduced sizes for faster testing)
    std::cout << "═══ Basic Operations ═══\n";
    results.push_back(bench_sequential_writes(5000));
    results.push_back(bench_random_writes(5000));
    results.push_back(bench_sequential_reads(5000));
    results.push_back(bench_random_reads(5000));
    results.push_back(bench_updates(2500));
    results.push_back(bench_deletions(2500));
    std::cout << "\n";

    // Mixed workload
    std::cout << "═══ Mixed Workload ═══\n";
    results.push_back(bench_mixed_workload(5000));
    std::cout << "\n";

    // Compaction
    std::cout << "═══ Compaction ═══\n";
    results.push_back(bench_compaction(8));
    std::cout << "\n";

    // Value sizes
    std::cout << "═══ Variable Value Sizes ═══\n";
    results.push_back(bench_large_values(500, 1024));
    results.push_back(bench_large_values(500, 10240));
    results.push_back(bench_large_values(50, 102400));
    std::cout << "\n";

    // Recovery
    std::cout << "═══ Recovery ═══\n";
    results.push_back(bench_recovery(2500));
    std::cout << "\n";

    // Summary
    std::cout << "╔════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                              Summary Results                               ║\n";
    std::cout << "╠════════════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ Benchmark                              │   Ops      │  Time (ms) │    Ops/s     │ Latency(µs)║\n";
    std::cout << "╠════════════════════════════════════════════════════════════════════════════╣\n";

    for (const auto& result : results) {
        std::cout << "║ ";
        result.print();
    }

    std::cout << "╚════════════════════════════════════════════════════════════════════════════╝\n";

    // Cleanup
    if (fs::exists("./bench_data")) {
        fs::remove_all("./bench_data");
    }

    std::cout << "\n✅ Benchmark complete!\n\n";

    // Performance analysis
    std::cout << "📊 Performance Analysis:\n";
    std::cout << "   - Write performance: " << results[0].ops_per_sec << " ops/s (sequential)\n";
    std::cout << "   - Read performance: " << results[2].ops_per_sec << " ops/s (sequential)\n";
    std::cout << "   - Random read penalty: "
              << (results[2].ops_per_sec / results[3].ops_per_sec) << "x slower\n";
    std::cout << "\n💡 Optimization Opportunities:\n";
    std::cout << "   1. Add Bloom filters - would speed up random reads significantly\n";
    std::cout << "   2. Add block-based index - would reduce SSTable scan time\n";
    std::cout << "   3. Implement LSM levels - would reduce read amplification\n";
    std::cout << "   4. Add background compaction - would eliminate write stalls\n";
    std::cout << "\n";

    return 0;
}