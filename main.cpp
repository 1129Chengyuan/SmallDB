#include <iostream>
#include <chrono>
#include <random>
#include <vector>
#include <filesystem>
#include <iomanip>
#include <numeric>
#include <algorithm>
#include <functional>
#include <cmath>
#include "kv_engine.h"

namespace fs = std::filesystem;

// Same memtable size for every engine (RocksDB won't go below 64KB)
constexpr size_t MEMTABLE_BYTES = 64 * 1024;
// Large enough that the recovery benchmark never flushes to an SSTable
constexpr size_t RECOVERY_MEMTABLE_BYTES = 1000000;
const std::string BENCH_DIR = "./bench_data";

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

    std::vector<std::string> sequential_keys(size_t n, size_t mod = 0) {
        std::vector<std::string> keys;
        keys.reserve(n);
        for (size_t i = 0; i < n; i++) {
            keys.push_back("key_" + std::to_string(mod ? i % mod : i));
        }
        return keys;
    }

    std::vector<std::string> random_keys(size_t n) {
        std::vector<std::string> keys;
        keys.reserve(n);
        for (size_t i = 0; i < n; i++) keys.push_back(random_key(16));
        return keys;
    }

    std::vector<std::string> random_values(size_t n, size_t length = 100) {
        std::vector<std::string> values;
        values.reserve(n);
        for (size_t i = 0; i < n; i++) values.push_back(random_value(length));
        return values;
    }

    std::vector<size_t> random_indices(size_t n, size_t max) {
        std::vector<size_t> indices;
        indices.reserve(n);
        for (size_t i = 0; i < n; i++) indices.push_back(random_int(max));
        return indices;
    }

private:
    std::mt19937 gen;
    std::uniform_int_distribution<> dist;
};

// An engine under test: a name plus a way to open it on a directory
struct Engine {
    std::string name;
    std::function<std::unique_ptr<kv_engine>(const std::string& dir, size_t memtable_bytes)> open;
};

void cleanup_bench_data() {
    if (fs::exists(BENCH_DIR)) {
        fs::remove_all(BENCH_DIR);
    }
    fs::create_directories(BENCH_DIR);
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

// Only `body` is timed; `setup` (load data, open db) and `teardown` (close db) are not
BenchmarkResult run_benchmark_averaged(const std::string& name, size_t ops,
                                       const std::function<void()>& setup,
                                       const std::function<void()>& body,
                                       const std::function<void()>& teardown,
                                       size_t num_runs = 10) {
    std::cout << "Running: " << name << " (" << num_runs << " runs)..." << std::flush;

    std::vector<double> durations;
    Timer timer;

    for (size_t run = 0; run < num_runs; ++run) {
        setup();
        timer.start();
        body();
        double duration = timer.elapsed_ms();
        teardown();
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

// Write every key/value, then close so the data is reopened from disk
void load(const Engine& engine, const std::vector<std::string>& keys,
          const std::vector<std::string>& values, size_t memtable_bytes = MEMTABLE_BYTES) {
    auto db = engine.open(BENCH_DIR, memtable_bytes);
    for (size_t i = 0; i < keys.size(); i++) {
        db->put(keys[i], values[i]);
    }
    db->wait_idle();
}

// ============================================================================
// Benchmark Tests
// ============================================================================

BenchmarkResult bench_writes(const Engine& engine, const std::string& name,
                             const std::vector<std::string>& keys,
                             const std::vector<std::string>& values,
                             size_t num_runs) {
    std::unique_ptr<kv_engine> db;

    return run_benchmark_averaged(name, keys.size(), [&]() {
        cleanup_bench_data();
        db = engine.open(BENCH_DIR, MEMTABLE_BYTES);
    }, [&]() {
        for (size_t i = 0; i < keys.size(); i++) {
            db->put(keys[i], values[i]);
        }
    }, [&]() {
        db.reset();
    }, num_runs);
}

BenchmarkResult bench_sequential_writes(const Engine& engine, size_t num_ops, size_t num_runs) {
    DataGenerator gen;
    auto keys = gen.sequential_keys(num_ops);
    auto values = gen.random_values(num_ops);
    return bench_writes(engine, "Sequential Writes", keys, values, num_runs);
}

BenchmarkResult bench_random_writes(const Engine& engine, size_t num_ops, size_t num_runs) {
    DataGenerator gen;
    auto keys = gen.random_keys(num_ops);
    auto values = gen.random_values(num_ops);
    return bench_writes(engine, "Random Writes", keys, values, num_runs);
}

BenchmarkResult bench_reads(const Engine& engine, const std::string& name,
                            const std::vector<std::string>& keys,
                            const std::vector<size_t>& read_order,
                            size_t num_runs) {
    DataGenerator gen;
    auto values = gen.random_values(keys.size());
    std::unique_ptr<kv_engine> db;

    return run_benchmark_averaged(name, read_order.size(), [&]() {
        cleanup_bench_data();
        load(engine, keys, values);
        db = engine.open(BENCH_DIR, MEMTABLE_BYTES);
    }, [&]() {
        for (size_t idx : read_order) {
            volatile std::string value = db->get(keys[idx]);
            (void)value;
        }
    }, [&]() {
        db.reset();
    }, num_runs);
}

BenchmarkResult bench_sequential_reads(const Engine& engine, size_t num_ops, size_t num_runs) {
    DataGenerator gen;
    auto keys = gen.sequential_keys(num_ops);
    std::vector<size_t> order(num_ops);
    std::iota(order.begin(), order.end(), 0);
    return bench_reads(engine, "Sequential Reads", keys, order, num_runs);
}

BenchmarkResult bench_random_reads(const Engine& engine, size_t num_ops, size_t num_runs) {
    DataGenerator gen;
    auto keys = gen.sequential_keys(num_ops);
    auto order = gen.random_indices(num_ops, num_ops);
    return bench_reads(engine, "Random Reads", keys, order, num_runs);
}

BenchmarkResult bench_mixed_workload(const Engine& engine, size_t num_ops, size_t num_runs) {
    DataGenerator gen;
    // Pre-populate num_ops/2 keys, then alternate writes (key_i) and random reads
    auto initial_keys = gen.sequential_keys(num_ops / 2);
    auto initial_values = gen.random_values(initial_keys.size());
    auto write_keys = gen.sequential_keys(num_ops);
    auto write_values = gen.random_values(num_ops);
    auto read_order = gen.random_indices(num_ops, initial_keys.size());
    std::unique_ptr<kv_engine> db;

    return run_benchmark_averaged("Mixed Workload (50% R/W)", num_ops, [&]() {
        cleanup_bench_data();
        load(engine, initial_keys, initial_values);
        db = engine.open(BENCH_DIR, MEMTABLE_BYTES);
    }, [&]() {
        for (size_t i = 0; i < num_ops; i++) {
            if (i % 2 == 0) {
                db->put(write_keys[i], write_values[i]);
            } else {
                volatile std::string value = db->get(initial_keys[read_order[i]]);
                (void)value;
            }
        }
    }, [&]() {
        db.reset();
    }, num_runs);
}

BenchmarkResult bench_updates(const Engine& engine, size_t num_ops, size_t num_runs) {
    DataGenerator gen;
    auto initial_keys = gen.sequential_keys(1000);
    auto initial_values = gen.random_values(1000);
    auto update_keys = gen.sequential_keys(num_ops, 1000);
    auto update_values = gen.random_values(num_ops);
    std::unique_ptr<kv_engine> db;

    return run_benchmark_averaged("Updates (Overwrites)", num_ops, [&]() {
        cleanup_bench_data();
        load(engine, initial_keys, initial_values);
        db = engine.open(BENCH_DIR, MEMTABLE_BYTES);
    }, [&]() {
        for (size_t i = 0; i < num_ops; i++) {
            db->put(update_keys[i], update_values[i]);
        }
    }, [&]() {
        db.reset();
    }, num_runs);
}

BenchmarkResult bench_deletions(const Engine& engine, size_t num_ops, size_t num_runs) {
    DataGenerator gen;
    auto keys = gen.sequential_keys(num_ops);
    auto values = gen.random_values(num_ops);
    std::unique_ptr<kv_engine> db;

    return run_benchmark_averaged("Deletions", num_ops, [&]() {
        cleanup_bench_data();
        load(engine, keys, values);
        db = engine.open(BENCH_DIR, MEMTABLE_BYTES);
    }, [&]() {
        for (const auto& key : keys) {
            db->remove(key);
        }
    }, [&]() {
        db.reset();
    }, num_runs);
}

BenchmarkResult bench_compaction(const Engine& engine, size_t num_memtables, size_t num_runs) {
    DataGenerator gen;
    // Roughly num_memtables memtables' worth of 100-byte entries
    size_t num_entries = num_memtables * MEMTABLE_BYTES / 110;
    auto keys = gen.sequential_keys(num_entries);
    auto values = gen.random_values(num_entries);
    std::unique_ptr<kv_engine> db;

    return run_benchmark_averaged("Full Compaction (" + std::to_string(num_entries) + " keys)", 1, [&]() {
        cleanup_bench_data();
        db = engine.open(BENCH_DIR, MEMTABLE_BYTES);
        for (size_t i = 0; i < keys.size(); i++) {
            db->put(keys[i], values[i]);
        }
        db->wait_idle();
    }, [&]() {
        db->compact();
    }, [&]() {
        db.reset();
    }, num_runs);
}

BenchmarkResult bench_large_values(const Engine& engine, size_t num_ops, size_t value_size, size_t num_runs) {
    DataGenerator gen;
    auto keys = gen.sequential_keys(num_ops);
    auto values = gen.random_values(num_ops, value_size);
    return bench_writes(engine, "Large Values (" + std::to_string(value_size) + " bytes)",
                        keys, values, num_runs);
}

BenchmarkResult bench_recovery(const Engine& engine, size_t num_ops, size_t num_runs) {
    DataGenerator gen;
    auto keys = gen.sequential_keys(num_ops);
    auto values = gen.random_values(num_ops);
    std::unique_ptr<kv_engine> db;

    return run_benchmark_averaged("WAL Recovery (" + std::to_string(num_ops) + " entries)", num_ops, [&]() {
        cleanup_bench_data();
        // Write to WAL without flushing, then snapshot the directory while the
        // db is still open. A clean close may flush the memtable (SmallDB does),
        // so restoring the snapshot simulates a crash and forces a WAL replay.
        const std::string snapshot = BENCH_DIR + "_crash";
        fs::remove_all(snapshot);
        {
            auto writer = engine.open(BENCH_DIR, RECOVERY_MEMTABLE_BYTES);
            for (size_t i = 0; i < keys.size(); i++) {
                writer->put(keys[i], values[i]);
            }
            fs::copy(BENCH_DIR, snapshot, fs::copy_options::recursive);
        }
        fs::remove_all(BENCH_DIR);
        fs::rename(snapshot, BENCH_DIR);
    }, [&]() {
        // Recovery from WAL
        db = engine.open(BENCH_DIR, RECOVERY_MEMTABLE_BYTES);
    }, [&]() {
        db.reset();
    }, num_runs);
}

// ============================================================================
// Main Benchmark Suite
// ============================================================================

std::vector<BenchmarkResult> run_suite(const Engine& engine) {
    const size_t NUM_RUNS = 10;
    std::vector<BenchmarkResult> results;

    std::cout << "\n######## " << engine.name << " ########\n\n";

    // Basic operations
    std::cout << "═══ Basic Operations ═══\n";
    results.push_back(bench_sequential_writes(engine, 5000, NUM_RUNS));
    results.push_back(bench_random_writes(engine, 5000, NUM_RUNS));
    results.push_back(bench_sequential_reads(engine, 5000, NUM_RUNS));
    results.push_back(bench_random_reads(engine, 5000, NUM_RUNS));
    results.push_back(bench_updates(engine, 2500, NUM_RUNS));
    results.push_back(bench_deletions(engine, 2500, NUM_RUNS));
    std::cout << "\n";

    // Mixed workload
    std::cout << "═══ Mixed Workload ═══\n";
    results.push_back(bench_mixed_workload(engine, 5000, NUM_RUNS));
    std::cout << "\n";

    // Compaction
    std::cout << "═══ Compaction ═══\n";
    results.push_back(bench_compaction(engine, 8, NUM_RUNS));
    std::cout << "\n";

    // Value sizes (fewer runs for large values)
    std::cout << "═══ Variable Value Sizes ═══\n";
    results.push_back(bench_large_values(engine, 500, 1024, 5));
    results.push_back(bench_large_values(engine, 500, 10240, 5));
    results.push_back(bench_large_values(engine, 50, 102400, 3));
    std::cout << "\n";

    // Recovery
    std::cout << "═══ Recovery ═══\n";
    results.push_back(bench_recovery(engine, 2500, NUM_RUNS));
    std::cout << "\n";

    // Summary
    std::cout << "╔═══════════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║ " << std::setw(82) << std::left << (engine.name + " Results") << "║\n";
    std::cout << "╠═══════════════════════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║ Benchmark                         │   Ops    │  Time (ms) │    Ops/s   │ Lat(µs) │ StdDev  ║\n";
    std::cout << "╠═══════════════════════════════════════════════════════════════════════════════════╣\n";

    for (const auto& result : results) {
        std::cout << "║ ";
        result.print();
    }

    std::cout << "╚═══════════════════════════════════════════════════════════════════════════════════╝\n";

    return results;
}

int main() {
    std::cout << "\n";
    std::cout << "╔════════════════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    SmallDB Performance Benchmark Suite                     ║\n";
    std::cout << "║                         Apple M3 MacBook Air                               ║\n";
    std::cout << "║                        Averaged over 10 runs                               ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════════════════════╝\n";

    std::vector<Engine> engines = {
        {"SmallDB", [](const std::string& dir, size_t memtable_bytes) -> std::unique_ptr<kv_engine> {
            return std::make_unique<smalldb_engine>(dir, memtable_bytes);
        }},
#ifdef SMALLDB_WITH_ROCKSDB
        {"RocksDB", [](const std::string& dir, size_t memtable_bytes) -> std::unique_ptr<kv_engine> {
            return std::make_unique<rocksdb_engine>(dir, memtable_bytes);
        }},
#endif
    };

    std::vector<std::vector<BenchmarkResult>> all_results;
    for (const auto& engine : engines) {
        all_results.push_back(run_suite(engine));
    }

    // Cleanup
    if (fs::exists(BENCH_DIR)) {
        fs::remove_all(BENCH_DIR);
    }

    std::cout << "\n✅ Benchmark complete!\n\n";

    // Performance analysis
    const auto& results = all_results[0];
    std::cout << "📊 SmallDB Performance Analysis:\n";
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

    // Head-to-head comparison
    if (all_results.size() > 1) {
        std::cout << "\n⚔️  SmallDB vs " << engines[1].name << " (ops/s, higher is better):\n";
        std::cout << "   " << std::setw(35) << std::left << "Benchmark"
                  << std::setw(14) << std::right << "SmallDB"
                  << std::setw(14) << engines[1].name
                  << std::setw(12) << "Ratio" << "\n";
        for (size_t i = 0; i < results.size(); i++) {
            const auto& a = all_results[0][i];
            const auto& b = all_results[1][i];
            std::cout << "   " << std::setw(35) << std::left << a.name
                      << std::setw(14) << std::right << std::setprecision(0) << a.ops_per_sec
                      << std::setw(14) << b.ops_per_sec
                      << std::setw(11) << std::setprecision(2) << (a.ops_per_sec / b.ops_per_sec) << "x\n";
        }
    }

    std::cout << "\n";

    return 0;
}
