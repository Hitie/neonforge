#include "data_internal.hpp"
#include "manifest.hpp"
#include <atomic>
#include <limits>
#include <mutex>

namespace nf::data
{
namespace detail
{
struct Rule
{
    Type type;
    bool required;
};
struct Schema
{
    std::map<std::string, Rule, std::less<>> fields;
};
struct Field
{
    Value value;
    std::vector<Edit> history;
};
struct Record
{
    std::string type;
    std::map<std::string, Field, std::less<>> fields;
};
struct State
{
    uint64_t revision = 0;
    std::map<std::string, std::shared_ptr<const Record>, std::less<>> records;
    std::string owner, source;
    std::set<std::string, std::less<>> changed;
};
struct Control
{
    std::thread::id thread = std::this_thread::get_id();
    Limits limits;
    std::map<std::string, Schema, std::less<>> schemas;
    std::shared_ptr<const State> state = std::make_shared<const State>();
    void check() const
    {
        require(thread == std::this_thread::get_id(), "database requires its creating thread");
    }
};
const Record &record(const State &state, std::string_view name)
{
    auto found = state.records.find(name);
    require(found != state.records.end(), "missing data record: " + std::string(name));
    return *found->second;
}
} // namespace detail
using namespace detail;
Snapshot::Snapshot(std::shared_ptr<const State> state) : state_(std::move(state)) {}
const State &Snapshot::live() const
{
    require(bool(state_), "data snapshot was moved from");
    return *state_;
}
uint64_t Snapshot::revision() const { return live().revision; }
size_t Snapshot::record_count() const { return live().records.size(); }
bool Snapshot::contains(std::string_view name) const { return live().records.contains(name); }
std::string_view Snapshot::record_type(std::string_view name) const
{
    return record(live(), name).type;
}
const Value &Snapshot::field(std::string_view name, std::string_view key) const
{
    const auto &fields = record(live(), name).fields;
    auto found = fields.find(key);
    require(found != fields.end(),
            "missing data field: " + std::string(name) + "." + std::string(key));
    return found->second.value;
}
std::vector<Edit> Snapshot::provenance(std::string_view name, std::string_view key) const
{
    (void)field(name, key);
    return record(live(), name).fields.find(key)->second.history;
}
Json Snapshot::json() const
{
    Json::Object records;
    for (const auto &[name, entry] : live().records)
    {
        Json::Object values, histories;
        for (const auto &[key, field] : entry->fields)
        {
            values.emplace(key, field.value.json());
            Json::Array edits;
            for (const auto &edit : field.history)
                edits.emplace_back(Json::Object{{"owner", string(edit.owner)},
                                                {"source", string(edit.source)},
                                                {"operation", string(edit.operation)},
                                                {"sequence", number(edit.sequence)}});
            histories.emplace(key, Json(edits));
        }
        records.emplace(name, Json(Json::Object{{"type", string(entry->type)},
                                                {"fields", Json(values)},
                                                {"provenance", Json(histories)}}));
    }
    return Json(Json::Object{{"format", Json(1.0)},
                             {"revision", number(live().revision)},
                             {"records", Json(records)},
                             {"commit", commit_info()},
                             {"engine_adapter_ready", Json(false)},
                             {"game_tested", Json(false)}});
}
Json Snapshot::commit_info() const
{
    Json::Array changed;
    for (const auto &name : live().changed)
        changed.emplace_back(name);
    return Json(Json::Object{{"owner", string(live().owner)},
                             {"source", string(live().source)},
                             {"changed_records", Json(changed)}});
}
struct Transaction::Impl
{
    std::weak_ptr<Control> control;
    std::shared_ptr<const State> base;
    std::shared_ptr<State> working;
    std::map<std::string, std::shared_ptr<Record>, std::less<>> changed;
    std::string owner, source;
    size_t operations = 0;
    bool closed = false, dirty = false;
    std::atomic_bool failed = false;
    std::mutex gate;
    std::thread::id thread = std::this_thread::get_id();
    std::shared_ptr<Control> check()
    {
        auto c = control.lock();
        if (!c)
        {
            failed = true;
            throw std::invalid_argument("database expired");
        }
        if (c->thread != std::this_thread::get_id())
        {
            failed = true;
            throw std::invalid_argument("database requires its creating thread");
        }
        require(!closed && !failed, "transaction is closed or failed");
        return c;
    }
    template <class F> auto mutate(F fn)
    {
        std::lock_guard lock(gate);
        auto c = check();
        try
        {
            require(++operations <= c->limits.operations, "data transaction operation limit");
            return fn(*c);
        }
        catch (...)
        {
            failed = true;
            throw;
        }
    }
    Record &edit(std::string_view name)
    {
        auto found = changed.find(name);
        if (found != changed.end())
            return *found->second;
        auto copy = std::make_shared<Record>(record(*working, name));
        auto key = std::string(name);
        changed.emplace(key, copy);
        working->records.insert_or_assign(key, copy);
        return *copy;
    }
    void history(Field &field, const Control &c, std::string operation)
    {
        require(field.history.size() < c.limits.history, "data field provenance limit");
        field.history.push_back({owner, source, std::move(operation), operations});
    }
    void put(std::string_view name, std::string key, Value value, Control &c, std::string operation)
    {
        identifier(key);
        auto &entry = edit(name);
        const auto &schema = c.schemas.at(entry.type);
        auto rule = schema.fields.find(key);
        require(rule != schema.fields.end(), "unknown field: " + std::string(name) + "." + key);
        require(rule->second.type == value.type(),
                "type mismatch: " + std::string(name) + "." + key);
        auto existing = entry.fields.find(key);
        Field field{std::move(value), existing == entry.fields.end() ? std::vector<Edit>{}
                                                                     : existing->second.history};
        history(field, c, std::move(operation));
        entry.fields.insert_or_assign(std::move(key), std::move(field));
        dirty = true;
        working->changed.emplace(name);
    }
};
Transaction::Transaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Transaction::Impl &Transaction::live()
{
    require(bool(impl_), "transaction was moved from");
    return *impl_;
}
Transaction::~Transaction() = default;
Transaction::Transaction(Transaction &&) noexcept = default;
Transaction &Transaction::operator=(Transaction &&) noexcept = default;
void Transaction::create(std::string name, std::string type)
{
    live().mutate(
        [&](Control &c)
        {
            identifier(name, 256);
            identifier(type);
            require(c.schemas.contains(type), "unknown record schema: " + type);
            require(!live().working->records.contains(name), "record already exists: " + name);
            auto entry = std::make_shared<Record>(Record{std::move(type), {}});
            live().working->records.emplace(name, entry);
            live().changed.emplace(name, entry);
            live().working->changed.emplace(name);
            live().dirty = true;
        });
}
void Transaction::clone(std::string name, std::string_view source)
{
    live().mutate(
        [&](Control &c)
        {
            identifier(name, 256);
            require(!live().working->records.contains(name), "record already exists: " + name);
            auto entry = std::make_shared<Record>(record(*live().working, source));
            for (auto &[key, field] : entry->fields)
            {
                (void)key;
                live().history(field, c, "clone");
            }
            live().working->records.emplace(name, entry);
            live().changed.emplace(name, entry);
            live().working->changed.emplace(name);
            live().dirty = true;
        });
}
void Transaction::set(std::string_view name, std::string key, Value value)
{
    live().mutate([&](Control &c)
                  { live().put(name, std::move(key), std::move(value), c, "set"); });
}
void Transaction::erase_record(std::string_view name)
{
    live().mutate(
        [&](Control &)
        {
            (void)record(*live().working, name);
            live().working->records.erase(std::string(name));
            live().changed.erase(std::string(name));
            live().working->changed.emplace(name);
            live().dirty = true;
        });
}
void Transaction::erase_field(std::string_view name, std::string_view key)
{
    live().mutate(
        [&](Control &)
        {
            auto &entry = live().edit(name);
            require(entry.fields.erase(std::string(key)) == 1,
                    "missing field: " + std::string(key));
            live().working->changed.emplace(name);
            live().dirty = true;
        });
}
bool Transaction::append(std::string_view name, std::string_view key, Value value, bool unique,
                         bool prepend)
{
    return live().mutate(
        [&](Control &c)
        {
            const auto &entry = record(*live().working, name);
            auto found = entry.fields.find(key);
            require(found != entry.fields.end(), "missing array field: " + std::string(key));
            const auto &current = found->second.value;
            require(current.type().kind() == "array" && current.type().element() == value.type(),
                    "array type mismatch: " + std::string(key));
            auto encoded = current.json().object();
            auto values = current.json().at("value").array();
            if (unique)
                for (const auto &item : values)
                    if (Value::parse(item) == value)
                        return false;
            if (prepend)
                values.insert(values.begin(), value.json());
            else
                values.push_back(value.json());
            encoded.insert_or_assign("value", Json(values));
            live().put(name, std::string(key), Value::parse(Json(encoded)), c,
                       prepend ? "prepend" : "append");
            return true;
        });
}
size_t Transaction::remove(std::string_view name, std::string_view key, const Value &value)
{
    return live().mutate(
        [&](Control &c) -> size_t
        {
            const auto &entry = record(*live().working, name);
            auto found = entry.fields.find(key);
            require(found != entry.fields.end(), "missing array field: " + std::string(key));
            const auto &current = found->second.value;
            require(current.type().kind() == "array" && current.type().element() == value.type(),
                    "array type mismatch: " + std::string(key));
            auto encoded = current.json().object();
            auto values = current.json().at("value").array();
            auto before = values.size();
            std::erase_if(values, [&](const Json &item) { return Value::parse(item) == value; });
            auto removed = before - values.size();
            if (removed)
            {
                encoded.insert_or_assign("value", Json(values));
                live().put(name, std::string(key), Value::parse(Json(encoded)), c, "remove");
            }
            return removed;
        });
}
uint64_t Transaction::commit()
{
    std::lock_guard lock(live().gate);
    auto c = live().check();
    try
    {
        require(c->state == live().base, "stale data transaction revision conflict");
        for (auto i = live().working->changed.begin(); i != live().working->changed.end();)
        {
            auto old = live().base->records.find(*i);
            auto now = live().working->records.find(*i);
            bool equal = old == live().base->records.end() && now == live().working->records.end();
            if (old != live().base->records.end() && now != live().working->records.end())
            {
                const auto &a = *old->second, &b = *now->second;
                equal = a.type == b.type && a.fields.size() == b.fields.size();
                if (equal)
                    for (const auto &[key, value] : a.fields)
                    {
                        auto field = b.fields.find(key);
                        if (field == b.fields.end() || !(field->second.value == value.value) ||
                            field->second.history != value.history)
                        {
                            equal = false;
                            break;
                        }
                    }
                if (equal)
                    live().working->records.insert_or_assign(*i, old->second);
            }
            if (equal)
            {
                live().changed.erase(*i);
                i = live().working->changed.erase(i);
            }
            else
                ++i;
        }
        if (live().working->changed.empty())
        {
            live().closed = true;
            live().changed.clear();
            live().working.reset();
            live().base.reset();
            return c->state->revision;
        }
        require(live().working->records.size() <= c->limits.records, "data record budget");
        size_t bytes = 0;
        auto account = [&](size_t size)
        {
            require(size <= c->limits.bytes - bytes, "data retained byte budget");
            bytes += size;
        };
        account(live().working->owner.size());
        account(live().working->source.size());
        for (const auto &name : live().working->changed)
            account(name.size());
        for (const auto &[name, entry] : live().working->records)
        {
            account(name.size());
            account(entry->type.size());
            require(entry->fields.size() <= c->limits.fields_per_record,
                    "data field budget: " + name);
            for (const auto &[key, rule] : c->schemas.at(entry->type).fields)
                require(!rule.required || entry->fields.contains(key),
                        "required field missing: " + name + "." + key);
            for (const auto &[key, field] : entry->fields)
            {
                account(key.size());
                account(field.value.encoded().size());
                for (const auto &edit : field.history)
                {
                    account(edit.owner.size());
                    account(edit.source.size());
                    account(edit.operation.size());
                    account(sizeof(edit.sequence));
                }
            }
        }
        if (live().dirty)
        {
            require(live().base->revision != UINT64_MAX, "data revision exhausted");
            live().working->revision = live().base->revision + 1;
            c->state = live().working;
        }
        live().closed = true;
        live().changed.clear();
        live().working.reset();
        live().base.reset();
        return c->state->revision;
    }
    catch (...)
    {
        live().failed = true;
        throw;
    }
}
void Transaction::abort()
{
    auto &state = live();
    std::lock_guard lock(state.gate);
    if (state.thread != std::this_thread::get_id())
    {
        state.failed = true;
        throw std::invalid_argument("transaction requires its creating thread");
    }
    require(!state.closed, "transaction is closed");
    state.closed = true;
    state.changed.clear();
    state.working.reset();
    state.base.reset();
}
Database::Database(const Json &schemas, Limits limits) : control_(std::make_shared<Control>())
{
    require(limits.records && limits.records <= 65536 && limits.fields_per_record &&
                limits.fields_per_record <= 4096 && limits.bytes && limits.bytes <= 64 * 1048576 &&
                limits.operations && limits.operations <= 16384 && limits.history &&
                limits.history <= 64,
            "invalid data limits");
    control_->limits = limits;
    require(schemas.object().size() <= 4096, "data schema count limit");
    for (const auto &[name, definition] : schemas.object())
    {
        identifier(name);
        fields(definition, {"fields"});
        Schema schema;
        require(definition.at("fields").object().size() <= limits.fields_per_record,
                "schema field budget");
        for (const auto &[key, descriptor] : definition.at("fields").object())
        {
            identifier(key);
            auto type = descriptor.object();
            bool required = false;
            if (auto value = descriptor.find("required"))
            {
                required = value->boolean();
                type.erase("required");
            }
            schema.fields.emplace(key, Rule{Type::parse(Json(type)), required});
        }
        control_->schemas.emplace(name, std::move(schema));
    }
}
Snapshot Database::snapshot() const
{
    control_->check();
    return Snapshot(control_->state);
}
Transaction Database::begin(std::string owner, std::string source)
{
    control_->check();
    require(owner.size() <= 128 && valid_id(owner), "invalid data owner");
    text(source, 240);
    auto impl = std::make_unique<Transaction::Impl>();
    impl->control = control_;
    impl->base = control_->state;
    impl->working = std::make_shared<State>(*impl->base);
    impl->working->changed.clear();
    impl->working->owner = owner;
    impl->working->source = source;
    impl->owner = std::move(owner);
    impl->source = std::move(source);
    return Transaction(std::move(impl));
}
} // namespace nf::data
