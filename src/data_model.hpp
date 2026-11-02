#pragma once
#include "json.hpp"
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>

namespace nf::data
{
class Type
{
    Json descriptor_;
    std::string key_;
    explicit Type(Json);

  public:
    static Type parse(const Json &);
    const Json &json() const { return descriptor_; }
    std::string_view kind() const { return descriptor_.at("type").string(); }
    Type element() const;
    bool operator==(const Type &other) const { return key_ == other.key_; }
};
class Value
{
    struct Rep;
    std::shared_ptr<const Rep> rep_;
    const Rep &live() const;
    explicit Value(std::shared_ptr<const Rep>);

  public:
    static Value parse(const Json &);
    const Type &type() const;
    const Json &json() const;
    const std::string &encoded() const;
    bool operator==(const Value &) const;
};
struct Limits
{
    size_t records = 4096, fields_per_record = 256, bytes = 16 * 1048576, operations = 4096,
           history = 64;
};
struct Edit
{
    std::string owner, source, operation;
    uint64_t sequence = 0;
    bool operator==(const Edit &) const = default;
};
namespace detail
{
struct State;
struct Control;
} // namespace detail
class Snapshot
{
    std::shared_ptr<const detail::State> state_;
    const detail::State &live() const;
    explicit Snapshot(std::shared_ptr<const detail::State>);
    friend class Database;

  public:
    uint64_t revision() const;
    size_t record_count() const;
    bool contains(std::string_view) const;
    std::string_view record_type(std::string_view) const;
    const Value &field(std::string_view record, std::string_view field) const;
    std::vector<Edit> provenance(std::string_view record, std::string_view field) const;
    Json json() const;
    Json commit_info() const;
};
class Transaction
{
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Impl &live();
    explicit Transaction(std::unique_ptr<Impl>);
    friend class Database;

  public:
    ~Transaction();
    Transaction(Transaction &&) noexcept;
    Transaction &operator=(Transaction &&) noexcept;
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;
    void create(std::string record, std::string type);
    void clone(std::string record, std::string_view source);
    void set(std::string_view record, std::string field, Value value);
    void erase_record(std::string_view record);
    void erase_field(std::string_view record, std::string_view field);
    bool append(std::string_view record, std::string_view field, Value value, bool unique = false,
                bool prepend = false);
    size_t remove(std::string_view record, std::string_view field, const Value &value);
    uint64_t commit();
    void abort();
};
class Database
{
    std::shared_ptr<detail::Control> control_;

  public:
    explicit Database(const Json &schemas, Limits = {});
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    Snapshot snapshot() const;
    Transaction begin(std::string owner, std::string source = {});
};
Json apply_project(const Json &, bool plan = false);
} // namespace nf::data
