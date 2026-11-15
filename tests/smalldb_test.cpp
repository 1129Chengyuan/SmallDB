// Correctness tests for SmallDB. Each test targets a specific bug that used to
// exist; the model test cross-checks everything against std::map.
//
// Build & run:  cmake --build build && ./build/smalldb_tests

#include "smalldb.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

static int failures = 0;

// std::string convenience wrapper (slice has no const char* constructor)
class test_db {
public:
  test_db(const std::string& dir, size_t memtable_bytes, bool background)
      : db_(dir, memtable_bytes, background) {}
  void put(const std::string& k, const std::string& v) { db_.put(slice(k), slice(v)); }
  void remove(const std::string& k) { db_.remove(slice(k)); }
  std::string get(const std::string& k) const { return db_.get(slice(k)); }
  void compact() { db_.compact(); }
  void wait_for_compaction() { db_.wait_for_compaction(); }
  size_t get_num_sstables() const { return db_.get_num_sstables(); }
  size_t get_memtable_size() const { return db_.get_memtable_size(); }

private:
  smalldb db_;
};

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cerr << "  CHECK FAILED " << __FILE__ << ":" << __LINE__ << ": "    \
                << #cond << std::endl;                                         \
      failures++;                                                              \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                         \
  do {                                                                         \
    auto _a = (a);                                                             \
    auto _b = (b);                                                             \
    if (!(_a == _b)) {                                                         \
      std::cerr << "  CHECK_EQ FAILED " << __FILE__ << ":" << __LINE__ << ": " \
                << #a << " = \"" << _a << "\", expected \"" << _b << "\""      \
                << std::endl;                                                  \
      failures++;                                                              \
    }                                                                          \
  } while (0)

// Fresh, empty directory per test
static std::string test_dir(const std::string& name) {
  fs::path dir = fs::temp_directory_path() /
                 ("smalldb_test_" + std::to_string(getpid())) / name;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

// Copy a directory while the DB is still open: restoring it is equivalent to a
// crash (no destructor flush). Only call when no compaction is running.
static void snapshot_dir(const std::string& from, const std::string& to) {
  fs::remove_all(to);
  fs::copy(from, to, fs::copy_options::recursive);
}

static void append_bytes(const std::string& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::app);
  out.write(bytes.data(), bytes.size());
}

// ---------------------------------------------------------------------------

// Bug: a tombstone in the memtable read as "not found" and fell through to an
// older SSTable, resurrecting the deleted value until the next flush.
static void test_delete_in_memtable_shadows_sstable() {
  std::string dir = test_dir("delete_shadow");
  test_db db(dir, 100, false);

  db.put("k", std::string(200, 'v'));  // exceeds threshold -> flushed
  CHECK_EQ(db.get_num_sstables(), size_t(1));

  db.remove("k");                      // tombstone stays in memtable
  CHECK_EQ(db.get_memtable_size(), size_t(1));
  CHECK_EQ(db.get("k"), std::string(""));
}

// Bug: after a compaction crashed between writing its output and deleting its
// inputs, restart loaded both. The output had dropped the tombstone, so the
// stale input's value came back. Uncommitted outputs were also loaded.
static void test_crash_mid_compaction_does_not_resurrect() {
  std::string dir = test_dir("crash_compaction");
  std::string saved = dir + "_saved_sst0";

  {
    test_db db(dir, 100, false);
    db.put("k", std::string(200, '1'));  // -> sstable_0 holds k=111...
  }
  fs::remove_all(saved);
  fs::create_directories(saved);
  fs::copy(dir + "/sstable_0.sst", saved + "/sstable_0.sst");
  fs::copy(dir + "/sstable_0.idx", saved + "/sstable_0.idx");

  {
    test_db db(dir, 100, false);
    db.remove("k");
    db.put("pad", std::string(200, 'p'));  // -> sstable_1 holds tombstone(k), pad
    db.compact();                          // -> sstable_2 holds pad only; 0,1 deleted
    CHECK_EQ(db.get("k"), std::string(""));
  }

  // Simulate the crash window: an old input survived deletion...
  fs::copy(saved + "/sstable_0.sst", dir + "/sstable_0.sst");
  fs::copy(saved + "/sstable_0.idx", dir + "/sstable_0.idx");
  // ...and an uncommitted output (never recorded in MANIFEST) exists too
  fs::copy(saved + "/sstable_0.sst", dir + "/sstable_50.sst");

  {
    test_db db(dir, 100, false);
    CHECK_EQ(db.get("k"), std::string(""));
    CHECK_EQ(db.get("pad"), std::string(200, 'p'));
    CHECK_EQ(db.get_num_sstables(), size_t(1));
  }
  CHECK(!fs::exists(dir + "/sstable_0.sst"));
  CHECK(!fs::exists(dir + "/sstable_50.sst"));
  fs::remove_all(saved);
}

// Bug: a torn/garbage WAL tail produced a huge key_size, and recovery
// allocated a string that size and crashed.
static void test_wal_garbage_tail() {
  std::string dir = test_dir("wal_garbage");
  std::string crashed = dir + "_crashed";

  {
    test_db db(dir, 1 << 20, false);  // big memtable: everything stays in WAL
    for (int i = 0; i < 50; i++) db.put("key" + std::to_string(i), "val" + std::to_string(i));
    snapshot_dir(dir, crashed);
  }
  append_bytes(crashed + "/wal.log", std::string(40, '\xFF'));

  std::string crashed2 = crashed + "2";
  {
    test_db db(crashed, 1 << 20, false);
    for (int i = 0; i < 50; i++) {
      CHECK_EQ(db.get("key" + std::to_string(i)), "val" + std::to_string(i));
    }
    // Writes after recovery must land after the last good record, not after
    // the garbage, or they'd be unreadable on the next recovery.
    db.put("after", "x");
    snapshot_dir(crashed, crashed2);
  }
  {
    test_db db(crashed2, 1 << 20, false);
    CHECK_EQ(db.get("after"), std::string("x"));
    CHECK_EQ(db.get("key49"), std::string("val49"));
  }
  fs::remove_all(crashed);
  fs::remove_all(crashed2);
}

// A record cut off mid-write (crash during append) is dropped; earlier ones survive.
static void test_wal_torn_last_record() {
  std::string dir = test_dir("wal_torn");
  std::string crashed = dir + "_crashed";
  {
    test_db db(dir, 1 << 20, false);
    db.put("a", "1");
    db.put("b", "2");
    snapshot_dir(dir, crashed);
  }
  std::string wal = crashed + "/wal.log";
  fs::resize_file(wal, fs::file_size(wal) - 1);  // tear the last record
  {
    test_db db(crashed, 1 << 20, false);
    CHECK_EQ(db.get("a"), std::string("1"));
    CHECK_EQ(db.get("b"), std::string(""));
  }
  fs::remove_all(crashed);
}

// Bit flips inside a record are caught by the checksum.
static void test_wal_checksum_detects_corruption() {
  std::string dir = test_dir("wal_crc");
  std::string crashed = dir + "_crashed";
  {
    test_db db(dir, 1 << 20, false);
    db.put("a", "hello");
    db.put("b", "world");
    snapshot_dir(dir, crashed);
  }
  std::string wal = crashed + "/wal.log";
  {
    // Flip the last byte ("d" of "world")
    std::fstream f(wal, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(-1, std::ios::end);
    f.put('X');
  }
  {
    test_db db(crashed, 1 << 20, false);
    CHECK_EQ(db.get("a"), std::string("hello"));
    CHECK_EQ(db.get("b"), std::string(""));  // not "worlX"
  }
  fs::remove_all(crashed);
}

// Randomized test against std::map. With background compaction running
// concurrently with flushes, this catches the old bugs where compaction
// replaced the whole table list (losing tables flushed mid-merge) and where
// two threads could generate the same SSTable file name.
static void model_test(bool background) {
  std::string dir = test_dir(background ? "model_bg" : "model_fg");
  std::mt19937 rng(background ? 42 : 7);
  std::map<std::string, std::string> model;

  auto db = std::make_unique<test_db>(dir, 2048, background);

  auto verify_all = [&](const char* when) {
    int bad = 0;
    for (int k = 0; k < 400; k++) {
      std::string key = "key" + std::to_string(k);
      auto it = model.find(key);
      std::string expected = it == model.end() ? "" : it->second;
      if (db->get(key) != expected) bad++;
    }
    if (bad) {
      std::cerr << "  model mismatch (" << when << "): " << bad << " keys" << std::endl;
      failures++;
    }
  };

  const int OPS = 30000;
  for (int i = 1; i <= OPS; i++) {
    std::string key = "key" + std::to_string(rng() % 400);
    int op = rng() % 10;
    if (op < 6) {
      std::string value = "v" + std::to_string(i) + std::string(rng() % 40, 'x');
      db->put(key, value);
      model[key] = value;
    } else if (op < 8) {
      db->remove(key);
      model.erase(key);
    } else {
      auto it = model.find(key);
      std::string expected = it == model.end() ? "" : it->second;
      if (db->get(key) != expected) {
        std::cerr << "  model mismatch at op " << i << " for " << key << std::endl;
        failures++;
        return;
      }
    }

    if (i % 2000 == 0) verify_all("while compacting");
    if (i % 7500 == 0) {
      db->wait_for_compaction();
      verify_all("after wait");
      db.reset();  // clean close
      db = std::make_unique<test_db>(dir, 2048, background);
      verify_all("after reopen");
    }
  }
  db->wait_for_compaction();
  db->compact();
  verify_all("after final compaction");
}

int main() {
  // Engine logging is noisy; keep test output readable
  std::streambuf* original = std::cout.rdbuf();
  std::ofstream devnull("/dev/null");

  struct Test { const char* name; std::function<void()> fn; };
  Test tests[] = {
      {"delete in memtable shadows SSTable", test_delete_in_memtable_shadows_sstable},
      {"crash mid-compaction does not resurrect", test_crash_mid_compaction_does_not_resurrect},
      {"WAL garbage tail", test_wal_garbage_tail},
      {"WAL torn last record", test_wal_torn_last_record},
      {"WAL checksum detects corruption", test_wal_checksum_detects_corruption},
      {"model test (inline compaction)", [] { model_test(false); }},
      {"model test (background compaction)", [] { model_test(true); }},
  };

  for (const auto& t : tests) {
    int before = failures;
    std::cout.rdbuf(devnull.rdbuf());
    try {
      t.fn();
    } catch (const std::exception& e) {
      std::cerr << "  EXCEPTION: " << e.what() << std::endl;
      failures++;
    }
    std::cout.rdbuf(original);
    std::cout << (failures == before ? "[PASS] " : "[FAIL] ") << t.name << std::endl;
  }

  fs::remove_all(fs::temp_directory_path() / ("smalldb_test_" + std::to_string(getpid())));
  std::cout << (failures ? "FAILED" : "ALL PASSED") << std::endl;
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
