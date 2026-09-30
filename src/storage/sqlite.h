#pragma once
#include "keyhunt/core/exact_range.h"
#include <sqlite3.h>
#include <filesystem>
#include <string>
#include <vector>

namespace keyhunt::storage::detail {
using Bytes=std::vector<uint8_t>;
Bytes digest(Bytes data);
Bytes random_bytes(size_t count);
std::string uuid();
std::filesystem::path state_path(const std::string& explicit_directory={});

// All SQL values are bound. A statement owns its SQLite handle and its column
// copies; no pointer into sqlite3_column_blob survives another step/finalize.
class Statement {
public:
    Statement(sqlite3* db,const std::string& sql);
    ~Statement();
    Statement(const Statement&)=delete;
    Statement& operator=(const Statement&)=delete;
    void bind(int i,const std::string& value);
    void bind(int i,const Bytes& value);
    void bind(int i,const core::UInt256& value);
    void bind(int i,int64_t value);
    bool step();
    Bytes blob(int i) const;
    std::string text(int i) const;
    int64_t integer(int i) const;
    core::UInt256 wide(int i) const;
private:
    sqlite3* db_; sqlite3_stmt* stmt_=nullptr;
};
class Database {
public:
    explicit Database(const std::string& directory={});
    ~Database();
    Database(const Database&)=delete;
    Database& operator=(const Database&)=delete;
    sqlite3* handle() const { return db_; }
    const std::filesystem::path& directory() const { return directory_; }
    void exec(const std::string& sql);
    Bytes metadata(const std::string& key) const;
    void metadata(const std::string& key,const Bytes& value);
    void writable() const;
    void backup(const std::string& destination_directory) const;
    static void restore(const std::string& source_directory,const std::string& destination_directory);
    void check() const;
private:
    std::filesystem::path directory_;
    sqlite3* db_=nullptr;
};
// BEGIN IMMEDIATE serializes selection with the resulting assignments. A failed
// operation or commit rolls back; no acknowledgement escapes before COMMIT.
class Transaction {
public:
    explicit Transaction(Database& db,bool write=true);
    ~Transaction();
    void commit();
private:
    Database& db_; bool active_=true;
};
} // namespace keyhunt::storage::detail
