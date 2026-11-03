#include "data_internal.hpp"
#include "manifest.hpp"
#include <atomic>
#include <charconv>
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
    Json schema_json;
    std::map<std::string, Schema, std::less<>> schemas;
    std::shared_ptr<const State> state = std::make_shared<const State>();
    void check() const
    {
        require(thread == std::this_thread::get_id(), "database requires its creating thread");
    }
};
struct CheckpointNodes
{
    size_t remaining = 262144;
    void consume(size_t count)
    {
        require(count <= remaining, "data checkpoint node limit");
        remaining -= count;
    }
    void json(const Json &input, size_t depth = 0)
    {
        require(depth <= 64, "data checkpoint JSON depth limit");
        consume(1);
        if (auto array = std::get_if<Json::Array>(&input.value))
            for (const auto &value : *array)
                json(value, depth + 1);
        if (auto object = std::get_if<Json::Object>(&input.value))
            for (const auto &[key, value] : *object)
            {
                (void)key;
                json(value, depth + 1);
            }
    }
};
const Record &record(const State &state, std::string_view name)
{
    auto found = state.records.find(name);
    require(found != state.records.end(), "missing data record: " + std::string(name));
    return *found->second;
}
void validate_state(const Control &c, const State &state)
{
    require(state.records.size() <= c.limits.records, "data record budget");
    size_t bytes = 0;
    auto account = [&](size_t size)
    {
        require(size <= c.limits.bytes - bytes, "data retained byte budget");
        bytes += size;
    };
    account(state.owner.size());
    account(state.source.size());
    for (const auto &name : state.changed)
        account(name.size());
    for (const auto &[name, entry] : state.records)
    {
        account(name.size());
        account(entry->type.size());
        require(entry->fields.size() <= c.limits.fields_per_record, "data field budget: " + name);
        for (const auto &[key, rule] : c.schemas.at(entry->type).fields)
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
        validate_state(*c, *live().working);
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
    auto resolved = resolve_schemas(schemas, limits);
    control_->schema_json = resolved;
    for (const auto &[name, definition] : resolved.object())
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
Json Database::checkpoint() const
{
    control_->check();
    // Count existing immutable values before materializing an expanded JSON tree.
    // The count matches the wire envelope, including history/metadata objects.
    CheckpointNodes nodes;
    nodes.consume(13 + control_->state->changed.size());
    nodes.json(control_->schema_json);
    for (const auto &[name, entry] : control_->state->records)
    {
        (void)name;
        nodes.consume(4);
        for (const auto &[key, field] : entry->fields)
        {
            (void)key;
            nodes.json(field.value.json());
            nodes.consume(1 + 5 * field.history.size());
        }
    }
    Json output(Json::Object{{"format", Json(1.0)},
                             {"kind", string("neonforge.offline-data-checkpoint")},
                             {"schemas", control_->schema_json},
                             {"state", snapshot().json()}});
    require(to_json(output).size() <= 32 * 1048576, "data checkpoint byte limit");
    return output;
}
void Database::restore_checkpoint(const Json &input)
{
    control_->check();
    // All input is untrusted; publish only after structural, type and budget checks.
    fields(input, {"format", "kind", "schemas", "state"});
    CheckpointNodes{}.json(input);
    require(exact_small_integer(input.at("format"), 1) &&
                input.at("kind").string() == "neonforge.offline-data-checkpoint",
            "unsupported data checkpoint format");
    require(to_json(input).size() <= 32 * 1048576, "data checkpoint byte limit");
    require(to_json(resolve_schemas(input.at("schemas"), control_->limits)) ==
                to_json(control_->schema_json),
            "checkpoint schemas do not match database");
    auto counter = [](const Json &encoded)
    {
        auto value = Value::parse(Json(Json::Object{{"type", string("u64")}, {"value", encoded}}));
        uint64_t result = 0;
        const auto &source = value.json().at("value").string();
        auto parsed = std::from_chars(source.data(), source.data() + source.size(), result);
        require(parsed.ec == std::errc{} && parsed.ptr == source.data() + source.size(),
                "invalid checkpoint counter");
        return result;
    };
    auto owner = [](const std::string &name)
    { require(name.size() <= 128 && valid_id(name), "invalid checkpoint owner"); };
    const auto &encoded = input.at("state");
    fields(encoded,
           {"format", "revision", "records", "commit", "engine_adapter_ready", "game_tested"});
    require(exact_small_integer(encoded.at("format"), 1) &&
                !encoded.at("engine_adapter_ready").boolean() &&
                !encoded.at("game_tested").boolean(),
            "unsupported checkpoint state flags or format");
    auto candidate = std::make_shared<State>();
    candidate->revision = counter(encoded.at("revision"));
    const auto &commit = encoded.at("commit");
    fields(commit, {"owner", "source", "changed_records"});
    candidate->owner = commit.at("owner").string();
    candidate->source = commit.at("source").string();
    text(candidate->source, 240);
    if (candidate->revision)
        owner(candidate->owner);
    else
        require(candidate->owner.empty() && candidate->source.empty(),
                "invalid initial checkpoint metadata");
    require(commit.at("changed_records").array().size() <= 16384,
            "checkpoint changed-record limit");
    for (const auto &name : commit.at("changed_records").array())
    {
        identifier(name.string(), 256);
        require(candidate->changed.insert(name.string()).second,
                "duplicate checkpoint changed record");
    }
    require(candidate->revision ? !candidate->changed.empty() : candidate->changed.empty(),
            "invalid checkpoint commit metadata");
    require(encoded.at("records").object().size() <= control_->limits.records,
            "data record budget");
    require(candidate->revision || encoded.at("records").object().empty(),
            "initial checkpoint has records");
    for (const auto &[name, record_json] : encoded.at("records").object())
    {
        identifier(name, 256);
        fields(record_json, {"type", "fields", "provenance"});
        auto entry = std::make_shared<Record>();
        entry->type = record_json.at("type").string();
        identifier(entry->type);
        auto schema = control_->schemas.find(entry->type);
        require(schema != control_->schemas.end(),
                "unknown checkpoint record schema: " + entry->type);
        const auto &values = record_json.at("fields").object();
        const auto &histories = record_json.at("provenance").object();
        require(values.size() <= control_->limits.fields_per_record &&
                    values.size() == histories.size(),
                "checkpoint field/provenance shape mismatch");
        for (const auto &[key, value_json] : values)
        {
            identifier(key);
            auto rule = schema->second.fields.find(key);
            require(rule != schema->second.fields.end(),
                    "unknown checkpoint field: " + name + "." + key);
            auto value = Value::parse(value_json);
            require(rule->second.type == value.type(),
                    "checkpoint field type mismatch: " + name + "." + key);
            auto history = histories.find(key);
            require(history != histories.end(),
                    "missing checkpoint provenance: " + name + "." + key);
            require(!history->second.array().empty() &&
                        history->second.array().size() <= control_->limits.history,
                    "checkpoint provenance limit");
            std::vector<Edit> edits;
            for (const auto &edit_json : history->second.array())
            {
                fields(edit_json, {"owner", "source", "operation", "sequence"});
                Edit edit{edit_json.at("owner").string(), edit_json.at("source").string(),
                          edit_json.at("operation").string(), counter(edit_json.at("sequence"))};
                owner(edit.owner);
                text(edit.source, 240);
                require(edit.operation == "set" || edit.operation == "clone" ||
                            edit.operation == "append" || edit.operation == "prepend" ||
                            edit.operation == "remove",
                        "invalid checkpoint edit operation");
                require(edit.sequence > 0 && edit.sequence <= 16384,
                        "invalid checkpoint edit sequence");
                require(!edits.empty() || edit.operation == "set",
                        "checkpoint field history must begin with set");
                require((edit.operation != "append" && edit.operation != "prepend" &&
                         edit.operation != "remove") ||
                            value.type().kind() == "array",
                        "checkpoint array operation on scalar field");
                if (candidate->revision == 1)
                {
                    require(edit.owner == candidate->owner && edit.source == candidate->source,
                            "first-revision checkpoint attribution mismatch");
                    require(edits.empty() || edits.back().sequence < edit.sequence,
                            "first-revision checkpoint sequence order");
                }
                edits.push_back(std::move(edit));
            }
            entry->fields.emplace(key, Field{std::move(value), std::move(edits)});
        }
        candidate->records.emplace(name, std::move(entry));
    }
    if (candidate->revision == 1)
    {
        require(candidate->changed.size() == candidate->records.size(),
                "first-revision checkpoint changed-record mismatch");
        for (const auto &[name, entry] : candidate->records)
        {
            (void)entry;
            require(candidate->changed.contains(name),
                    "first-revision checkpoint missing changed record");
        }
    }
    validate_state(*control_, *candidate);
    // A new state identity also invalidates transactions begun before a restore.
    control_->state = std::move(candidate);
}
} // namespace nf::data
