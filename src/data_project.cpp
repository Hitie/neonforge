#include "data_internal.hpp"

namespace nf::data
{
using namespace detail;
namespace
{
struct PlanBudget
{
    size_t entries = 0, bytes = 0;
    void add(size_t count)
    {
        require(count <= 4096 - entries, "expanded data plan entry limit");
        entries += count;
    }
    void row(const Json &json)
    {
        auto size = to_json(json).size();
        require(size <= 4 * 1048576 - bytes, "expanded data plan byte limit");
        bytes += size;
    }
};
struct Template
{
    std::string type;
    std::map<std::string, Value, std::less<>> values;
};
using Templates = std::map<std::string, Template, std::less<>>;
void apply_operation_impl(Transaction &tx, const Json &operation, const Templates &templates)
{
    const auto &op = operation.at("op").string();
    const auto &record = operation.at("record").string();
    if (op == "create")
    {
        fields(operation, {"op", "record", "type"});
        tx.create(record, operation.at("type").string());
    }
    else if (op == "instantiate")
    {
        fields(operation, {"op", "record", "template", "fields"});
        const auto &definition = templates.at(operation.at("template").string());
        auto values = definition.values;
        if (auto overrides = operation.find("fields"))
        {
            require(overrides->object().size() <= 256, "instance field limit");
            for (const auto &[key, value] : overrides->object())
                values.insert_or_assign(key, Value::parse(value));
        }
        tx.create(record, definition.type);
        for (const auto &[key, value] : values)
            tx.set(record, key, value);
    }
    else if (op == "clone")
    {
        fields(operation, {"op", "record", "from"});
        tx.clone(record, operation.at("from").string());
    }
    else if (op == "set")
    {
        fields(operation, {"op", "record", "field", "value"});
        tx.set(record, operation.at("field").string(), Value::parse(operation.at("value")));
    }
    else if (op == "erase_record")
    {
        fields(operation, {"op", "record"});
        tx.erase_record(record);
    }
    else if (op == "erase_field")
    {
        fields(operation, {"op", "record", "field"});
        tx.erase_field(record, operation.at("field").string());
    }
    else if (op == "append" || op == "prepend")
    {
        fields(operation, {"op", "record", "field", "value", "unique"});
        bool unique = false;
        if (auto j = operation.find("unique"))
            unique = j->boolean();
        tx.append(record, operation.at("field").string(), Value::parse(operation.at("value")),
                  unique, op == "prepend");
    }
    else if (op == "remove")
    {
        fields(operation, {"op", "record", "field", "value"});
        tx.remove(record, operation.at("field").string(), Value::parse(operation.at("value")));
    }
    else
        throw std::invalid_argument("unknown data operation: " + op);
}
void apply_operation(Transaction &tx, const Json &operation, const Templates &templates)
{
    std::string label = "data operation";
    try
    {
        if (auto value = operation.find("record"))
            if (auto name = std::get_if<std::string>(&value->value))
                label += " " + *name;
        if (auto value = operation.find("field"))
            if (auto name = std::get_if<std::string>(&value->value))
                label += "." + *name;
        if (auto value = operation.find("op"))
            if (auto name = std::get_if<std::string>(&value->value))
                label += " (" + *name + ")";
        apply_operation_impl(tx, operation, templates);
    }
    catch (const std::bad_variant_access &)
    {
        throw std::invalid_argument(label + ": wrong JSON kind");
    }
    catch (const std::exception &e)
    {
        throw std::invalid_argument(label + ": " + e.what());
    }
}
Json overwrite_report(const Snapshot &before, const Snapshot &after, const Json &schemas,
                      std::string_view owner, PlanBudget &budget)
{
    Json::Array result;
    if (before.revision() == after.revision())
        return Json(result);
    auto commit = after.commit_info();
    for (const auto &changed : commit.at("changed_records").array())
    {
        const auto &name = changed.string();
        if (!before.contains(name))
            continue;
        std::set<std::string, std::less<>> keys;
        for (const auto &[key, rule] : schemas.at(before.record_type(name)).at("fields").object())
        {
            (void)rule;
            keys.insert(key);
        }
        if (after.contains(name))
            for (const auto &[key, rule] :
                 schemas.at(after.record_type(name)).at("fields").object())
            {
                (void)rule;
                keys.insert(key);
            }
        for (const auto &key : keys)
        {
            std::vector<Edit> old_edits, new_edits;
            try
            {
                old_edits = before.provenance(name, key);
            }
            catch (const std::invalid_argument &)
            {
                continue;
            }
            if (after.contains(name))
                try
                {
                    new_edits = after.provenance(name, key);
                }
                catch (const std::invalid_argument &)
                {
                }
            if (old_edits.empty() || old_edits.back().owner == owner)
                continue;
            if (!new_edits.empty() && new_edits.back().owner != owner)
                continue;
            budget.add(1);
            result.emplace_back(Json::Object{
                {"record", string(name)},
                {"field", string(key)},
                {"previous_owner", string(old_edits.back().owner)},
                {"owner", string(std::string(owner))},
                {"action", string(new_edits.empty() ? "erase" : new_edits.back().operation)}});
        }
    }
    return Json(result);
}
} // namespace
static std::unique_ptr<Database> execute_project(const Json &project, bool plan, Json *output)
{
    fields(project, {"format", "schemas", "templates", "transactions"});
    require(exact_small_integer(project.at("format"), 1), "unsupported data project format");
    auto schemas = resolve_schemas(project.at("schemas"));
    auto definitions = resolve_templates(
        project.find("templates") ? project.at("templates") : Json(Json::Object{}), schemas);
    Templates templates;
    for (const auto &[name, definition] : definitions.object())
    {
        Template entry{definition.at("type").string(), {}};
        for (const auto &[key, value] : definition.at("fields").object())
            entry.values.emplace(key, Value::parse(value));
        templates.emplace(name, std::move(entry));
    }
    auto owned = std::make_unique<Database>(schemas);
    auto &database = *owned;
    Json::Array batches;
    PlanBudget budget;
    require(project.at("transactions").array().size() <= 4096, "data transaction count limit");
    for (const auto &batch : project.at("transactions").array())
    {
        fields(batch, {"owner", "source", "expect_revision", "operations"});
        const auto &owner = batch.at("owner").string();
        std::string source = batch.find("source") ? batch.at("source").string() : "";
        try
        {
            auto before = database.snapshot();
            if (auto expected = batch.find("expect_revision"))
            {
                Value::parse(Json(Json::Object{{"type", string("u64")}, {"value", *expected}}));
                require(expected->string() == std::to_string(before.revision()),
                        "declared revision conflict");
            }
            auto tx = database.begin(owner, source);
            for (const auto &operation : batch.at("operations").array())
                apply_operation(tx, operation, templates);
            auto revision = tx.commit();
            auto after = database.snapshot();
            if (plan)
            {
                auto item = after.commit_info().object();
                if (revision == before.revision())
                    item = Json::Object{{"owner", string(owner)},
                                        {"source", string(source)},
                                        {"changed_records", Json(Json::Array{})}};
                item.emplace("base_revision", number(before.revision()));
                item.emplace("revision", number(revision));
                budget.add(1 + item.at("changed_records").array().size());
                item.emplace("overwrites", overwrite_report(before, after, schemas, owner, budget));
                budget.row(Json(item));
                batches.emplace_back(std::move(item));
            }
        }
        catch (const std::exception &e)
        {
            throw std::invalid_argument("data transaction " + owner + " [" + source +
                                        "]: " + e.what());
        }
    }
    if (plan)
        *output = Json(Json::Object{{"format", Json(1.0)},
                                    {"mode", string("offline-plan")},
                                    {"transactions", Json(std::move(batches))},
                                    {"revision", number(database.snapshot().revision())},
                                    {"engine_adapter_ready", Json(false)},
                                    {"game_tested", Json(false)}});
    else if (output)
        *output = database.snapshot().json();
    return owned;
}
std::unique_ptr<Database> build_project(const Json &project)
{
    return execute_project(project, false, nullptr);
}
Json apply_project(const Json &project, bool plan)
{
    Json output;
    (void)execute_project(project, plan, &output);
    return output;
}
} // namespace nf::data
