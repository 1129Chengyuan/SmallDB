#include <iostream>
#include <cassert>
#include <filesystem>
#include "smalldb.h"
#include "slice.h"

namespace fs = std::filesystem;

// Test counter
int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(condition, message) \
  do { \
    if (condition) { \
      std::cout << "  ✓ " << message << std::endl; \
      tests_passed++; \
    } else { \
      std::cout << "  ✗ FAILED: " << message << std::endl; \
      tests_failed++; \
    } \
  } while(0)

// Clean up test data directory
void cleanup_test_data() {
  if (fs::exists("./test_data")) {
    fs::remove_all("./test_data");
  }
  fs::create_directories("./test_data");
}

void test_basic_put_get() {
  std::cout << "\n=== Test 1: Basic Put/Get ===" << std::endl;
  cleanup_test_data();

  smalldb db("./test_data", 1024);

  // Test simple put/get
  db.put(slice("key1"), slice("value1"));
  std::string result = db.get(slice("key1"));
  TEST_ASSERT(result == "value1", "Put and get single key");

  // Test multiple keys
  db.put(slice("key2"), slice("value2"));
  db.put(slice("key3"), slice("value3"));
  TEST_ASSERT(db.get(slice("key2")) == "value2", "Get second key");
  TEST_ASSERT(db.get(slice("key3")) == "value3", "Get third key");

  // Test non-existent key
  TEST_ASSERT(db.get(slice("nonexistent")) == "", "Non-existent key returns empty");
}

void test_updates() {
  std::cout << "\n=== Test 2: Updates ===" << std::endl;
  cleanup_test_data();

  smalldb db("./test_data", 1024);

  // Initial value
  db.put(slice("counter"), slice("1"));
  TEST_ASSERT(db.get(slice("counter")) == "1", "Initial value");

  // Update same key
  db.put(slice("counter"), slice("2"));
  TEST_ASSERT(db.get(slice("counter")) == "2", "Updated value (second write)");

  // Update again
  db.put(slice("counter"), slice("3"));
  TEST_ASSERT(db.get(slice("counter")) == "3", "Updated value (third write)");
}

void test_deletions() {
  std::cout << "\n=== Test 3: Deletions ===" << std::endl;
  cleanup_test_data();

  smalldb db("./test_data", 1024);

  // Put then delete
  db.put(slice("temp"), slice("temporary"));
  TEST_ASSERT(db.get(slice("temp")) == "temporary", "Value exists before delete");

  db.remove(slice("temp"));
  TEST_ASSERT(db.get(slice("temp")) == "", "Value removed after delete");

  // Delete non-existent key (should not crash)
  db.remove(slice("never_existed"));
  TEST_ASSERT(db.get(slice("never_existed")) == "", "Deleting non-existent key is safe");
}

void test_memtable_flush() {
  std::cout << "\n=== Test 4: MemTable Flush ===" << std::endl;
  cleanup_test_data();

  smalldb db("./test_data", 50);  // Very small threshold

  // Write enough data to trigger flush
  for (int i = 0; i < 10; i++) {
    std::string key = "key_" + std::to_string(i);
    std::string value = "value_" + std::to_string(i);
    db.put(slice(key), slice(value));
  }

  // Verify data is still accessible after flush
  TEST_ASSERT(db.get(slice("key_0")) == "value_0", "First key after flush");
  TEST_ASSERT(db.get(slice("key_5")) == "value_5", "Middle key after flush");
  TEST_ASSERT(db.get(slice("key_9")) == "value_9", "Last key after flush");

  // Check that SSTables were created
  TEST_ASSERT(db.get_num_sstables() > 0, "SSTables were created");
}

void test_compaction() {
  std::cout << "\n=== Test 5: Compaction ===" << std::endl;
  cleanup_test_data();

  smalldb db("./test_data", 50);

  // Write data in controlled batches to observe compaction
  // Write enough to create 4 SSTables (will trigger auto-compaction)
  std::cout << "  Writing initial batch..." << std::endl;
  for (int i = 0; i < 20; i++) {
    std::string key = "item_" + std::to_string(i);
    std::string value = "data_" + std::to_string(i);
    db.put(slice(key), slice(value));
  }

  // After auto-compaction, should have 1 SSTable
  size_t sstables_after_compaction = db.get_num_sstables();
  std::cout << "  SSTables after first compaction: " << sstables_after_compaction << std::endl;
  TEST_ASSERT(sstables_after_compaction <= 2, "Auto-compaction reduced SSTable count");

  // Verify all data is correct after compaction
  TEST_ASSERT(db.get(slice("item_0")) == "data_0", "Data intact after compaction");
  TEST_ASSERT(db.get(slice("item_10")) == "data_10", "Data intact after compaction");
  TEST_ASSERT(db.get(slice("item_19")) == "data_19", "Data intact after compaction");

  // Verify all keys from first batch
  int correct = 0;
  for (int i = 0; i < 20; i++) {
    std::string key = "item_" + std::to_string(i);
    std::string expected = "data_" + std::to_string(i);
    if (db.get(slice(key)) == expected) {
      correct++;
    }
  }
  TEST_ASSERT(correct == 20, "All 20 keys readable after compaction");
}

void test_compaction_with_updates() {
  std::cout << "\n=== Test 6: Compaction with Updates ===" << std::endl;
  cleanup_test_data();

  smalldb db("./test_data", 50);

  // Write initial data
  for (int i = 0; i < 20; i++) {
    std::string key = "key_" + std::to_string(i);
    db.put(slice(key), slice("old_value"));
  }

  // Update some keys (creates newer versions in different SSTables)
  for (int i = 0; i < 10; i++) {
    std::string key = "key_" + std::to_string(i);
    db.put(slice(key), slice("new_value"));
  }

  // After compaction, should only have newest values
  TEST_ASSERT(db.get(slice("key_0")) == "new_value", "Updated key has new value");
  TEST_ASSERT(db.get(slice("key_15")) == "old_value", "Non-updated key has old value");
}

void test_compaction_with_deletes() {
  std::cout << "\n=== Test 7: Compaction with Deletes ===" << std::endl;
  cleanup_test_data();

  smalldb db("./test_data", 50);

  // Write data
  for (int i = 0; i < 20; i++) {
    std::string key = "del_" + std::to_string(i);
    db.put(slice(key), slice("value"));
  }

  // Delete half of them
  for (int i = 0; i < 10; i++) {
    std::string key = "del_" + std::to_string(i);
    db.remove(slice(key));
  }

  // Trigger more writes to force compaction
  for (int i = 20; i < 40; i++) {
    std::string key = "del_" + std::to_string(i);
    db.put(slice(key), slice("value"));
  }

  // Deleted keys should still be gone
  TEST_ASSERT(db.get(slice("del_0")) == "", "Deleted key stays deleted after compaction");
  TEST_ASSERT(db.get(slice("del_5")) == "", "Deleted key stays deleted after compaction");

  // Non-deleted keys should still exist
  TEST_ASSERT(db.get(slice("del_15")) == "value", "Non-deleted key persists");
  TEST_ASSERT(db.get(slice("del_25")) == "value", "New key exists");
}

void test_persistence() {
  std::cout << "\n=== Test 8: Persistence & Recovery ===" << std::endl;
  cleanup_test_data();

  // Write data in first instance
  {
    smalldb db("./test_data", 1024);
    db.put(slice("persist1"), slice("value1"));
    db.put(slice("persist2"), slice("value2"));
    db.put(slice("persist3"), slice("value3"));
  } // db destroyed here

  // Reopen database and verify data persists
  {
    smalldb db("./test_data", 1024);
    TEST_ASSERT(db.get(slice("persist1")) == "value1", "Data persists after restart");
    TEST_ASSERT(db.get(slice("persist2")) == "value2", "Data persists after restart");
    TEST_ASSERT(db.get(slice("persist3")) == "value3", "Data persists after restart");
  }
}

void test_wal_recovery() {
  std::cout << "\n=== Test 9: WAL Recovery ===" << std::endl;
  cleanup_test_data();

  // Write data (will be in WAL and MemTable, but not flushed)
  {
    smalldb db("./test_data", 10000);  // Large threshold to prevent flush
    db.put(slice("wal1"), slice("value1"));
    db.put(slice("wal2"), slice("value2"));
    // Don't flush - data only in WAL and MemTable
  }

  // Reopen - should recover from WAL
  {
    smalldb db("./test_data", 10000);
    TEST_ASSERT(db.get(slice("wal1")) == "value1", "WAL recovery works");
    TEST_ASSERT(db.get(slice("wal2")) == "value2", "WAL recovery works");
  }
}

void test_large_dataset() {
  std::cout << "\n=== Test 10: Large Dataset ===" << std::endl;
  cleanup_test_data();

  smalldb db("./test_data", 200);

  // Write many keys
  const int num_keys = 200;
  for (int i = 0; i < num_keys; i++) {
    std::string key = "large_key_" + std::to_string(i);
    std::string value = "large_value_" + std::to_string(i) + "_with_extra_padding";
    db.put(slice(key), slice(value));
  }

  // Verify random samples
  TEST_ASSERT(db.get(slice("large_key_0")) == "large_value_0_with_extra_padding",
              "Large dataset - first key");
  TEST_ASSERT(db.get(slice("large_key_100")) == "large_value_100_with_extra_padding",
              "Large dataset - middle key");
  TEST_ASSERT(db.get(slice("large_key_199")) == "large_value_199_with_extra_padding",
              "Large dataset - last key");

  // Verify all keys
  int correct_count = 0;
  for (int i = 0; i < num_keys; i++) {
    std::string key = "large_key_" + std::to_string(i);
    std::string expected = "large_value_" + std::to_string(i) + "_with_extra_padding";
    if (db.get(slice(key)) == expected) {
      correct_count++;
    }
  }
  TEST_ASSERT(correct_count == num_keys,
              "All " + std::to_string(num_keys) + " keys readable");
}

void test_empty_values() {
  std::cout << "\n=== Test 11: Edge Cases - Empty Values ===" << std::endl;
  cleanup_test_data();

  smalldb db("./test_data", 1024);

  // Empty value
  db.put(slice("empty_key"), slice(""));
  TEST_ASSERT(db.get(slice("empty_key")) == "", "Empty value stored and retrieved");

  // Single character
  db.put(slice("single"), slice("x"));
  TEST_ASSERT(db.get(slice("single")) == "x", "Single character value");
}

void test_special_characters() {
  std::cout << "\n=== Test 12: Edge Cases - Special Characters ===" << std::endl;
  cleanup_test_data();

  smalldb db("./test_data", 1024);

  // Special characters in keys and values
  db.put(slice("key@#$"), slice("value!@#$%"));
  TEST_ASSERT(db.get(slice("key@#$")) == "value!@#$%", "Special characters in key/value");

  // Unicode (if supported)
  db.put(slice("emoji"), slice("🎉"));
  TEST_ASSERT(db.get(slice("emoji")) == "🎉", "Unicode characters");
}

void test_compaction_manual_trigger() {
  std::cout << "\n=== Test 13: Manual Compaction ===" << std::endl;
  cleanup_test_data();

  smalldb db("./test_data", 50);

  // Create multiple SSTables but not enough to auto-trigger
  for (int i = 0; i < 15; i++) {
    std::string key = "comp_" + std::to_string(i);
    db.put(slice(key), slice("value"));
  }

  size_t before = db.get_num_sstables();

  // Manually trigger compaction
  db.compact();

  size_t after = db.get_num_sstables();
  TEST_ASSERT(after <= before, "Manual compaction reduced or maintained SSTable count");

  // Data should still be intact
  TEST_ASSERT(db.get(slice("comp_0")) == "value", "Data intact after manual compaction");
  TEST_ASSERT(db.get(slice("comp_14")) == "value", "Data intact after manual compaction");
}

int main() {
  std::cout << "╔════════════════════════════════════════╗" << std::endl;
  std::cout << "║   SmallDB Comprehensive Test Suite    ║" << std::endl;
  std::cout << "╚════════════════════════════════════════╝" << std::endl;

  test_basic_put_get();
  test_updates();
  test_deletions();
  test_memtable_flush();
  test_compaction();
  test_compaction_with_updates();
  test_compaction_with_deletes();
  test_persistence();
  test_wal_recovery();
  test_large_dataset();
  test_empty_values();
  test_special_characters();
  test_compaction_manual_trigger();

  std::cout << "\n╔════════════════════════════════════════╗" << std::endl;
  std::cout << "║           Test Summary                 ║" << std::endl;
  std::cout << "╠════════════════════════════════════════╣" << std::endl;
  std::cout << "║  Passed: " << tests_passed << " tests" << std::endl;
  std::cout << "║  Failed: " << tests_failed << " tests" << std::endl;
  std::cout << "╚════════════════════════════════════════╝" << std::endl;

  if (tests_failed == 0) {
    std::cout << "\n🎉 All tests passed! Your database works correctly!" << std::endl;
  } else {
    std::cout << "\n❌ Some tests failed. Review the output above." << std::endl;
  }

  // Cleanup
  if (fs::exists("./test_data")) {
    fs::remove_all("./test_data");
  }

  return tests_failed == 0 ? 0 : 1;
}