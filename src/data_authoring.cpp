#include "data_internal.hpp"
#include <functional>

namespace nf::data
{
using namespace detail;
namespace
{
// A bounded single-parent graph. Heights also constrain cached ancestors reached
// through a different path, so traversal order cannot relax the depth limit.
template <class Build> Json resolve(const Json &definitions, Build build)
{
    require(definitions.object().size() <= 4096, "data definition count limit");
    Json::Object result;
    std::map<std::string, size_t, std::less<>> heights;
    std::set<std::string, std::less<>> active;
    size_t total_fields = 0, total_bytes = 2;
    std::function<void(const std::string &, size_t)> visit;
    visit = [&](const std::string &name, size_t depth)
    {
        identifier(name);
        require(depth <= 32, "data inheritance depth limit: " + name);
        if (result.contains(name))
        {
            require(depth + heights.at(name) - 1 <= 32, "data inheritance depth limit: " + name);
            return;
        }
        auto found = definitions.object().find(name);
        require(found != definitions.object().end(), "missing data parent: " + name);
        require(active.insert(name).second, "data inheritance cycle: " + name);
        const Json *parent = nullptr;
        size_t height = 1;
        if (auto p = found->second.find("extends"))
        {
            const auto &base = p->string();
            visit(base, depth + 1);
            parent = &result.at(base);
            height += heights.at(base);
        }
        auto output = build(name, found->second, parent);
        size_t count = output.at("fields").object().size();
        require(count <= 16384 - total_fields, "expanded data field limit");
        total_fields += count;
        auto bytes = to_json(Json(Json::Object{{name, output}})).size() - (result.empty() ? 2 : 1);
        require(bytes <= 4 * 1048576 - total_bytes, "expanded data registry byte limit");
        total_bytes += bytes;
        heights.emplace(name, height);
        result.emplace(name, std::move(output));
        active.erase(name);
    };
    for (const auto &[name, definition] : definitions.object())
    {
        (void)definition;
        visit(name, 1);
    }
    return Json(std::move(result));
}
} // namespace
Json resolve_schemas(const Json &definitions, Limits limits)
{
    return resolve(
        definitions,
        [&](const std::string &name, const Json &definition, const Json *parent)
        {
            fields(definition, {"extends", "fields"});
            auto merged = parent ? parent->at("fields").object() : Json::Object{};
            require(definition.at("fields").object().size() <= limits.fields_per_record,
                    "schema field budget: " + name);
            for (const auto &[key, descriptor] : definition.at("fields").object())
            {
                identifier(key);
                require(!merged.contains(key), "inherited field shadowing: " + name + "." + key);
                auto type = descriptor.object();
                bool required =
                    descriptor.find("required") ? descriptor.at("required").boolean() : false;
                type.erase("required");
                auto normalized = Type::parse(Json(type)).json().object();
                normalized.emplace("required", Json(required));
                merged.emplace(key, Json(std::move(normalized)));
            }
            require(merged.size() <= limits.fields_per_record, "schema field budget: " + name);
            return Json(Json::Object{{"fields", Json(std::move(merged))}});
        });
}
Json resolve_templates(const Json &definitions, const Json &schemas)
{
    return resolve(
        definitions,
        [&](const std::string &name, const Json &definition, const Json *parent)
        {
            fields(definition, {"extends", "type", "fields"});
            std::string type = definition.find("type") ? definition.at("type").string()
                               : parent                ? parent->at("type").string()
                                                       : "";
            identifier(type);
            require(!parent || parent->at("type").string() == type,
                    "template cannot change inherited type: " + name);
            const auto &rules = schemas.at(type).at("fields").object();
            auto merged = parent ? parent->at("fields").object() : Json::Object{};
            require(definition.at("fields").object().size() <= 256, "template field limit");
            for (const auto &[key, encoded] : definition.at("fields").object())
            {
                identifier(key);
                auto rule = rules.find(key);
                require(rule != rules.end(), "unknown template field: " + name + "." + key);
                auto descriptor = rule->second.object();
                descriptor.erase("required");
                auto value = Value::parse(encoded);
                require(value.type() == Type::parse(Json(descriptor)),
                        "template field type mismatch: " + name + "." + key);
                merged.insert_or_assign(key, value.json());
            }
            return Json(Json::Object{{"type", string(type)}, {"fields", Json(std::move(merged))}});
        });
}
} // namespace nf::data
