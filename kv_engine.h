//
// Common key-value interface so the same benchmark can drive SmallDB and RocksDB.
//

#ifndef KV_ENGINE_H
#define KV_ENGINE_H

#include "smalldb.h"
#include "slice.h"
#include <memory>
#include <stdexcept>
#include <string>

#ifdef SMALLDB_WITH_ROCKSDB
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#endif

class kv_engine {
public:
  virtual ~kv_engine() = default;

  virtual void put(const std::string& key, const std::string& value) = 0;
  // Returns "" when the key is missing (matches smalldb::get)
  virtual std::string get(const std::string& key) = 0;
  virtual void remove(const std::string& key) = 0;
  // Full compaction, blocks until complete
  virtual void compact() = 0;
  // Wait for background work so it isn't billed to the next phase
  virtual void wait_idle() = 0;
};

class smalldb_engine : public kv_engine {
public:
  smalldb_engine(const std::string& dir, size_t memtable_bytes)
      : db_(dir, memtable_bytes) {}

  void put(const std::string& key, const std::string& value) override {
    db_.put(slice(key), slice(value));
  }
  std::string get(const std::string& key) override { return db_.get(slice(key)); }
  void remove(const std::string& key) override { db_.remove(slice(key)); }
  void compact() override { db_.compact(); }
  void wait_idle() override { db_.wait_for_compaction(); }

private:
  smalldb db_;
};

#ifdef SMALLDB_WITH_ROCKSDB
class rocksdb_engine : public kv_engine {
public:
  rocksdb_engine(const std::string& dir, size_t memtable_bytes) {
    rocksdb::Options options;
    options.create_if_missing = true;
    // Match SmallDB: same memtable size (RocksDB clamps to >= 64KB),
    // no compression, bloom filters on, small block cache.
    options.write_buffer_size = memtable_bytes;
    options.compression = rocksdb::kNoCompression;
    rocksdb::BlockBasedTableOptions table_options;
    table_options.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
    table_options.block_cache = rocksdb::NewLRUCache(1 << 20);
    options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options));

    check(rocksdb::DB::Open(options, dir, &db_));
    // Like SmallDB's WAL: written and flushed to the OS, but not fsync'd
    write_options_.sync = false;
  }

  void put(const std::string& key, const std::string& value) override {
    check(db_->Put(write_options_, key, value));
  }
  std::string get(const std::string& key) override {
    std::string value;
    rocksdb::Status s = db_->Get(read_options_, key, &value);
    if (s.IsNotFound()) return "";
    check(s);
    return value;
  }
  void remove(const std::string& key) override {
    check(db_->Delete(write_options_, key));
  }
  void compact() override {
    check(db_->CompactRange(rocksdb::CompactRangeOptions(), nullptr, nullptr));
  }
  void wait_idle() override {
    check(db_->WaitForCompact(rocksdb::WaitForCompactOptions()));
  }

private:
  static void check(const rocksdb::Status& s) {
    if (!s.ok()) throw std::runtime_error("rocksdb: " + s.ToString());
  }

  std::unique_ptr<rocksdb::DB> db_;
  rocksdb::WriteOptions write_options_;
  rocksdb::ReadOptions read_options_;
};
#endif

#endif // KV_ENGINE_H
