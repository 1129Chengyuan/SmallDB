// Correctness tests for SmallDB. Each test targets a specific bug that used to
// exist; the model test cross-checks everything against std::map.
//
// Build & run:  cmake --build build && ./build/smalldb_tests

#include "smalldb.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
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

  {
    test_db db(dir, 100, false);
    db.remove("k");
    db.put("pad", std::string(200, 'p'));  // -> sstable_1 holds tombstone(k), pad
    db.compact();                          // -> sstable_2 holds pad only; 0,1 deleted
    CHECK_EQ(db.get("k"), std::string(""));
  }

  // Simulate the crash window: an old input survived deletion...
  fs::copy(saved + "/sstable_0.sst", dir + "/sstable_0.sst");
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

// ---------------------------------------------------------------------------
// Table cache / ss_table lookup edge cases

static std::string key_name(int i) {
  char buf[16];
  snprintf(buf, sizeof(buf), "k%04d", i);
  return buf;
}

// Write a table straight from a memtable and open it
static std::shared_ptr<ss_table> make_table(const std::string& path,
                                            const std::map<std::string, std::string>& data) {
  mem_table mt(SIZE_MAX);
  for (const auto& [k, v] : data) mt.set(slice(k), slice(v));
  if (!ss_table::flush_memtable(mt, path)) throw std::runtime_error("flush failed");
  return ss_table::open(path);
}

static std::optional<std::string> lookup(const ss_table& t, const std::string& k) {
  return t.get(slice(k));
}

static std::string value_for(int i) {
  return "v" + std::to_string(i) + std::string(100, 'x');
}

// Every present key (including the first and last key of every block), every
// gap between keys, and keys before the first / after the last.
static void test_table_lookup_boundaries() {
  std::string dir = test_dir("table_bounds");
  std::map<std::string, std::string> data;
  for (int i = 0; i < 200; i += 2) data[key_name(i)] = value_for(i);  // 100 keys, ~12 KB
  auto t = make_table(dir + "/sstable_0.sst", data);

  CHECK(t->num_blocks() >= 3);  // spans several 4 KB blocks
  CHECK_EQ(t->num_entries(), uint64_t(100));
  for (int i = 0; i < 200; i++) {
    auto got = lookup(*t, key_name(i));
    if (i % 2 == 0) {
      CHECK(got.has_value() && *got == value_for(i));
    } else {
      CHECK(!got.has_value());
    }
    CHECK(!lookup(*t, key_name(i) + "x").has_value());  // just after each key
  }
  CHECK(!lookup(*t, "a").has_value());       // before smallest
  CHECK(!lookup(*t, "k").has_value());       // prefix of every key
  CHECK(!lookup(*t, "z").has_value());       // after largest
  CHECK(!lookup(*t, "k9999").has_value());   // after largest, same prefix
}

static void test_table_tiny_and_empty() {
  std::string dir = test_dir("table_tiny");
  auto empty = make_table(dir + "/sstable_0.sst", {});
  CHECK_EQ(empty->num_blocks(), size_t(0));
  CHECK(!lookup(*empty, "anything").has_value());
  CHECK(!lookup(*empty, "").has_value());

  auto one = make_table(dir + "/sstable_1.sst", {{"only", "1"}});
  CHECK(lookup(*one, "only") == std::optional<std::string>("1"));
  CHECK(!lookup(*one, "onlx").has_value());
  CHECK(!lookup(*one, "onlz").has_value());

  // Empty value is distinguishable from missing at this layer
  auto ev = make_table(dir + "/sstable_2.sst", {{"e", ""}});
  CHECK(lookup(*ev, "e") == std::optional<std::string>(""));
}

static void test_table_large_values() {
  std::string dir = test_dir("table_large");
  std::map<std::string, std::string> data;
  for (int i = 0; i < 40; i++) data[key_name(i)] = std::string(100 * 1024, char('a' + i % 26));
  auto t = make_table(dir + "/sstable_0.sst", data);
  CHECK_EQ(t->num_blocks(), size_t(40));  // each large record gets its own block
  for (int i = 0; i < 40; i++) {
    auto got = lookup(*t, key_name(i));
    CHECK(got.has_value() && got->size() == 100 * 1024 && (*got)[0] == char('a' + i % 26));
  }
}

// Run fn and report whether it threw
static bool throws(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const std::exception&) {
    return true;
  }
  return false;
}

static void flip_byte(const std::string& path, std::streamoff offset) {
  std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
  f.seekg(offset);
  char c;
  f.get(c);
  f.seekp(offset);
  f.put(static_cast<char>(c ^ 0x01));
}

// Checksums and the footer turn corruption into errors instead of wrong answers
static void test_table_corruption_detected() {
  std::string dir = test_dir("table_corrupt");
  std::map<std::string, std::string> data;
  for (int i = 0; i < 100; i++) data[key_name(i)] = value_for(i);

  // Bit flip inside the first data block: open still works (it doesn't read
  // data blocks), but reading that block fails its checksum
  std::string a = dir + "/sstable_0.sst";
  make_table(a, data);
  flip_byte(a, 20);
  auto t = ss_table::open(a);
  CHECK(throws([&] { lookup(*t, key_name(0)); }));
  CHECK(lookup(*t, key_name(99)) == std::optional<std::string>(value_for(99)));  // other blocks fine

  // Truncated file (crash before the footer was complete): bad magic
  std::string b = dir + "/sstable_1.sst";
  make_table(b, data);
  fs::resize_file(b, fs::file_size(b) - 1);
  CHECK(throws([&] { ss_table::open(b); }));

  // Bit flip in the footer's block handles: footer checksum mismatch
  std::string c = dir + "/sstable_2.sst";
  make_table(c, data);
  flip_byte(c, static_cast<std::streamoff>(fs::file_size(c) - ss_table::FOOTER_SIZE + 3));
  CHECK(throws([&] { ss_table::open(c); }));

  // Not an SSTable at all (or the old two-file format)
  std::string d = dir + "/sstable_3.sst";
  {
    std::ofstream out(d, std::ios::binary);
    out << std::string(200, 'z');
  }
  CHECK(throws([&] { ss_table::open(d); }));
}

// The bloom filter is stored on disk, so its hash must never change between
// builds. This pins the exact value (computed independently in Python).
static void test_bloom_filter_stable_and_persisted() {
  CHECK_EQ(bloom_filter::hash(slice(std::string("hello"))), uint64_t(0xb7f9172fac45c7e0ULL));

  bloom_filter f(1000, 0.01);
  for (int i = 0; i < 1000; i++) f.add(slice(key_name(i)));
  std::string bytes = f.serialize();
  auto g = bloom_filter::deserialize(bytes.data(), bytes.size());
  CHECK(g != nullptr);
  int false_positives = 0;
  for (int i = 0; i < 1000; i++) CHECK(g->contains(slice(key_name(i))));  // no false negatives
  for (int i = 1000; i < 11000; i++) false_positives += g->contains(slice(key_name(i)));
  CHECK(false_positives < 300);  // ~1% expected of 10,000
  CHECK(bloom_filter::deserialize(bytes.data(), bytes.size() - 1) == nullptr);
}

static void test_builder_rejects_unsorted_keys() {
  std::string dir = test_dir("builder_order");
  std::string path = dir + "/sstable_0.sst";
  {
    sstable_builder builder(path);
    builder.add(slice(std::string("b")), slice(std::string("1")));
    CHECK(throws([&] { builder.add(slice(std::string("a")), slice(std::string("2"))); }));
    CHECK(throws([&] { builder.add(slice(std::string("b")), slice(std::string("3"))); }));
  }  // never finished: destructor removes the partial file
  CHECK(!fs::exists(path));
}

// Bug: an unreadable compaction input used to be treated as an empty table,
// so compaction "succeeded" and deleted the input along with its data.
static void test_compaction_aborts_on_corrupt_input() {
  std::string dir = test_dir("compaction_corrupt");
  {
    test_db db(dir, 100, false);
    db.put("a", std::string(200, '1'));  // -> sstable_0
    db.put("b", std::string(200, '2'));  // -> sstable_1
  }
  flip_byte(dir + "/sstable_0.sst", 20);  // corrupt a data block of the table holding "a"

  test_db db(dir, 100, false);
  db.compact();                                   // must fail, not drop "a"
  CHECK_EQ(db.get_num_sstables(), size_t(2));     // inputs still live
  CHECK(fs::exists(dir + "/sstable_0.sst"));
  CHECK_EQ(db.get("b"), std::string(200, '2'));
}

// The table keeps its fd open: after compaction unlinks the file, a reader
// still holding the table can finish (POSIX unlink semantics).
static void test_table_readable_after_unlink() {
  std::string dir = test_dir("table_unlink");
  auto t = make_table(dir + "/sstable_0.sst", {{"a", "1"}, {"b", "2"}});
  fs::remove(dir + "/sstable_0.sst");
  CHECK(lookup(*t, "b") == std::optional<std::string>("2"));
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
      {"table: lookup boundaries", test_table_lookup_boundaries},
      {"table: tiny and empty tables", test_table_tiny_and_empty},
      {"table: large values", test_table_large_values},
      {"table: corruption detected (checksums, footer)", test_table_corruption_detected},
      {"bloom filter: stable hash, persisted", test_bloom_filter_stable_and_persisted},
      {"builder: rejects unsorted keys, cleans up", test_builder_rejects_unsorted_keys},
      {"compaction aborts on corrupt input", test_compaction_aborts_on_corrupt_input},
      {"table: readable after unlink", test_table_readable_after_unlink},
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
