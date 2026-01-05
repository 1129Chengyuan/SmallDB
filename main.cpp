#include <iostream>
#include <chrono>
#include <random>
#include <vector>
#include <filesystem>
#include <iomanip>
#include <numeric>
#include <algorithm>
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
    double std_dev;

    void print() const {
        std::cout << std::setw(35) << std::left << name << " | "
                  << std::setw(8) << std::right << operations << " ops | "
                  << std::setw(10) << std::fixed << std::setprecision(2)
                  << duration_ms << " ms | "
                  << std::setw(10) << std::fixed << std::setprecision(0)
                  << ops_per_sec << " ops/s | "
                  << std::setw(8) << std::fixed << std::setprecision(2)
                  << latency_us << " µs | "
                  << std::setw(8) << std::fixed << std::setprecision(2)
                  << std_dev << " ms" << std::endl;
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

// Calculate standard deviation
double calculate_std_dev(const std::vector<double>& values, double mean) {
    if (values.size() <= 1) return 0.0;

    double sum_sq_diff = 0.0;
    for (double val : values) {
        double diff = val - mean;
        sum_sq_diff += diff * diff;
    }
    return std::sqrt(sum_sq_diff / (values.size() - 1));
}

BenchmarkResult run_benchmark_averaged(const std::string& name, size_t ops,
                                       std::function<void()> fn, size_t num_runs = 10) {
    std::cout << "Running: " << name << " (" << num_runs << " runs)..." << std::flush;

    std::vector<double> durations;
    Timer timer;

    for (size_t run = 0; run < num_runs; ++run) {
        timer.start();
        fn();
        double duration = timer.elapsed_ms();
        durations.push_back(duration);
    }

    // Calculate statistics
    double avg_duration = std::accumulate(durations.begin(), durations.end(), 0.0) / durations.size();
    double std_dev = calculate_std_dev(durations, avg_duration);
    double min_duration = *std::min_element(durations.begin(), durations.end());
    double max_duration = *std::max_element(durations.begin(), durations.end());

    std::cout << " Done (avg: " << std::fixed << std::setprecision(2)
              << avg_duration << " ms, min: " << min_duration
              << " ms, max: " << max_duration << " ms)" << std::endl;

    return BenchmarkResult{
        name,
        ops,
        avg_duration,
        (ops * 1000.0) / avg_duration,
        (avg_duration * 1000.0) / ops,
        std_dev
    };
}

// ============================================================================
// Benchmark Tests
// ============================================================================

BenchmarkResult bench_sequential_writes(size_t num_ops, size_t num_runs) {
    DataGenerator gen;

    return run_benchmark_averaged("Sequential Writes", num_ops, [&]() {
        cleanup_bench_data();
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops; i++) {
            std::string key = "key_" + std::to_string(i);
            std::string value = gen.random_value(100);
            db.put(slice(key), slice(value));
        }
    }, num_runs);
}

BenchmarkResult bench_random_writes(size_t num_ops, size_t num_runs) {
    DataGenerator gen;

    return run_benchmark_averaged("Random Writes", num_ops, [&]() {
        cleanup_bench_data();
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops; i++) {
            std::string key = gen.random_key(16);
            std::string value = gen.random_value(100);
            db.put(slice(key), slice(value));
        }
    }, num_runs);
}

BenchmarkResult bench_sequential_reads(size_t num_ops, size_t num_runs) {
    DataGenerator gen;

    return run_benchmark_averaged("Sequential Reads", num_ops, [&]() {
        cleanup_bench_data();

        // Setup: Write data
        {
            smalldb db("./bench_data", 8192);
            for (size_t i = 0; i < num_ops; i++) {
                std::string key = "key_" + std::to_string(i);
                std::string value = gen.random_value(100);
                db.put(slice(key), slice(value));
            }
        }

        // Benchmark: Read data
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops; i++) {
            std::string key = "key_" + std::to_string(i);
            volatile std::string value = db.get(slice(key));
            (void)value;
        }
    }, num_runs);
}

BenchmarkResult bench_random_reads(size_t num_ops, size_t num_runs) {
    DataGenerator gen;

    return run_benchmark_averaged("Random Reads", num_ops, [&]() {
        cleanup_bench_data();

        std::vector<std::string> keys;

        // Setup: Write data
        {
            smalldb db("./bench_data", 8192);
            for (size_t i = 0; i < num_ops; i++) {
                std::string key = "key_" + std::to_string(i);
                keys.push_back(key);
                std::string value = gen.random_value(100);
                db.put(slice(key), slice(value));
            }
        }

        // Benchmark: Random reads
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops; i++) {
            size_t idx = gen.random_int(keys.size());
            volatile std::string value = db.get(slice(keys[idx]));
            (void)value;
        }
    }, num_runs);
}

BenchmarkResult bench_mixed_workload(size_t num_ops, size_t num_runs) {
    DataGenerator gen;

    return run_benchmark_averaged("Mixed Workload (50% R/W)", num_ops, [&]() {
        cleanup_bench_data();

        std::vector<std::string> keys;

        // Pre-populate some keys
        {
            smalldb db("./bench_data", 8192);
            for (size_t i = 0; i < num_ops / 2; i++) {
                std::string key = "key_" + std::to_string(i);
                keys.push_back(key);
                db.put(slice(key), slice(gen.random_value(100)));
            }
        }

        // Benchmark: 50% reads, 50% writes
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
    }, num_runs);
}

BenchmarkResult bench_updates(size_t num_ops, size_t num_runs) {
    DataGenerator gen;

    return run_benchmark_averaged("Updates (Overwrites)", num_ops, [&]() {
        cleanup_bench_data();

        // Setup: Initial writes
        {
            smalldb db("./bench_data", 8192);
            for (size_t i = 0; i < 1000; i++) {
                std::string key = "key_" + std::to_string(i);
                db.put(slice(key), slice(gen.random_value(100)));
            }
        }

        // Benchmark: Update existing keys
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops; i++) {
            std::string key = "key_" + std::to_string(i % 1000);
            db.put(slice(key), slice(gen.random_value(100)));
        }
    }, num_runs);
}

BenchmarkResult bench_deletions(size_t num_ops, size_t num_runs) {
    DataGenerator gen;

    return run_benchmark_averaged("Deletions", num_ops, [&]() {
        cleanup_bench_data();

        // Setup: Write data
        {
            smalldb db("./bench_data", 8192);
            for (size_t i = 0; i < num_ops; i++) {
                std::string key = "key_" + std::to_string(i);
                db.put(slice(key), slice(gen.random_value(100)));
            }
        }

        // Benchmark: Deletions
        smalldb db("./bench_data", 8192);
        for (size_t i = 0; i < num_ops; i++) {
            std::string key = "key_" + std::to_string(i);
            db.remove(slice(key));
        }
    }, num_runs);
}

BenchmarkResult bench_compaction(size_t num_sstables, size_t num_runs) {
    DataGenerator gen;

    return run_benchmark_averaged("Compaction (" + std::to_string(num_sstables) + " SSTables)", 1, [&]() {
        cleanup_bench_data();

        smalldb db("./bench_data", 100);
        for (size_t i = 0; i < num_sstables * 25; i++) {
            std::string key = "key_" + std::to_string(i);
            db.put(slice(key), slice(gen.random_value(100)));
        }

        db.compact();
    }, num_runs);
}

BenchmarkResult bench_large_values(size_t num_ops, size_t value_size, size_t num_runs) {
    DataGenerator gen;

    return run_benchmark_averaged("Large Values (" + std::to_string(value_size) + " bytes)", num_ops, [&]() {
        cleanup_bench_data();
        smalldb db("./bench_data", 16384);
        for (size_t i = 0; i < num_ops; i++) {
            std::string key = "key_" + std::to_string(i);
            std::string value = gen.random_value(value_size);
            db.put(slice(key), slice(value));
        }
    }, num_runs);
}

BenchmarkResult bench_recovery(size_t num_ops, size_t num_runs) {
    DataGenerator gen;

    return run_benchmark_averaged("WAL Recovery (" + std::to_string(num_ops) + " entries)", num_ops, [&]() {
        cleanup_bench_data();

        // Setup: Write to WAL without flushing
        {
            smalldb db("./bench_data", 1000000);
            for (size_t i = 0; i < num_ops; i++) {
                std::string key = "key_" + std::to_string(i);
                db.put(slice(key), slice(gen.random_value(100)));
            }
        }

        // Benchmark: Recovery from WAL
        smalldb db("./bench_data", 1000000);
    }, num_runs);
}

// ============================================================================
// Main Benchmark Suite
// ============================================================================

int main() {
    std::cout << "\n";
    std::cout << "╔════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    SmallDB Performance Benchmark Suite                     ║\n";
    std::cout << "║                         Apple M3 MacBook Air                               ║\n";
    std::cout << "║                        Averaged over 10 runs                               ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\n";

    const size_t NUM_RUNS = 10;
    std::vector<BenchmarkResult> results;

    // Basic operations
    std::cout << "═══ Basic Operations ═══\n";
    results.push_back(bench_sequential_writes(5000, NUM_RUNS));
    results.push_back(bench_random_writes(5000, NUM_RUNS));
    results.push_back(bench_sequential_reads(5000, NUM_RUNS));
    results.push_back(bench_random_reads(5000, NUM_RUNS));
    results.push_back(bench_updates(2500, NUM_RUNS));
    results.push_back(bench_deletions(2500, NUM_RUNS));
    std::cout << "\n";

    // Mixed workload
    std::cout << "═══ Mixed Workload ═══\n";
    results.push_back(bench_mixed_workload(5000, NUM_RUNS));
    std::cout << "\n";

    // Compaction
    std::cout << "═══ Compaction ═══\n";
    results.push_back(bench_compaction(8, NUM_RUNS));
    std::cout << "\n";

    // Value sizes (fewer runs for large values)
    std::cout << "═══ Variable Value Sizes ═══\n";
    results.push_back(bench_large_values(500, 1024, 5));
    results.push_back(bench_large_values(500, 10240, 5));
    results.push_back(bench_large_values(50, 102400, 3));
    std::cout << "\n";

    // Recovery
    std::cout << "═══ Recovery ═══\n";
    results.push_back(bench_recovery(2500, NUM_RUNS));
    std::cout << "\n";

    // Summary
    std::cout << "╔═══════════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                                  Summary Results                                  ║\n";
    std::cout << "╠═══════════════════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ Benchmark                         │   Ops    │  Time (ms) │    Ops/s   │ Lat(µs) │ StdDev  ║\n";
    std::cout << "╠═══════════════════════════════════════════════════════════════════════════════════╣\n";

    for (const auto& result : results) {
        std::cout << "║ ";
        result.print();
    }

    std::cout << "╚═══════════════════════════════════════════════════════════════════════════════════╝\n";

    // Cleanup
    if (fs::exists("./bench_data")) {
        fs::remove_all("./bench_data");
    }

    std::cout << "\n✅ Benchmark complete!\n\n";

    // Performance analysis
    std::cout << "📊 Performance Analysis:\n";
    std::cout << "   - Avg Write Performance: " << std::fixed << std::setprecision(0)
              << results[0].ops_per_sec << " ops/s (sequential)\n";
    std::cout << "   - Avg Read Performance: " << results[2].ops_per_sec << " ops/s (sequential)\n";
    std::cout << "   - Read/Write Ratio: " << std::fixed << std::setprecision(2)
              << (results[2].ops_per_sec / results[0].ops_per_sec) << "x\n";
    std::cout << "   - Random Read Penalty: "
              << (results[2].ops_per_sec / results[3].ops_per_sec) << "x slower\n";

    std::cout << "\n📉 Variability:\n";
    std::cout << "   - Sequential Writes StdDev: " << std::fixed << std::setprecision(2)
              << results[0].std_dev << " ms\n";
    std::cout << "   - Sequential Reads StdDev: " << results[2].std_dev << " ms\n";

    std::cout << "\n💡 Next Optimizations:\n";
    std::cout << "   1. Block-based index - reduce SSTable scan time\n";
    std::cout << "   2. Implement LSM levels - reduce read amplification\n";
    std::cout << "   3. Background compaction - eliminate write stalls\n";
    std::cout << "   4. Compression - reduce disk I/O\n";
    std::cout << "\n";

    return 0;
}