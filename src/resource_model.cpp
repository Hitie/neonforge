#include "resource_model.hpp"
#include "data_internal.hpp"
#include "manifest.hpp"
#include <charconv>
#include <functional>

namespace nf::resources
{
using namespace nf::data;
using namespace nf::data::detail;
namespace
{
const Json empty_object(Json::Object{}), empty_array(Json::Array{});
const Json &object_member(const Json &json, std::string_view key)
{
    auto value = json.find(key);
    return value ? *value : empty_object;
}
const Json &array_member(const Json &json, std::string_view key)
{
    auto value = json.find(key);
    return value ? *value : empty_array;
}
std::string path(const Json &json)
{
    const auto &value = json.string();
    require(safe_relative_path(value), "invalid resource path: " + value);
    for (char c : value)
        require((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '.' ||
                    c == '_' || c == '-',
                "resource path must be canonical lowercase ASCII: " + value);
    return value;
}
std::string language(const Json &json)
{
    const auto &value = json.string();
    require(value.size() >= 2 && value.size() <= 32, "invalid locale length");
    size_t length = 0, index = 0;
    for (char c : value)
    {
        if (c == '-')
        {
            require(index ? length >= 2 && length <= 8 : length >= 2 && length <= 3,
                    "invalid locale component");
            ++index;
            length = 0;
        }
        else
        {
            require((c >= 'a' && c <= 'z') || (index && c >= '0' && c <= '9'),
                    "locale must use canonical lowercase tags");
            ++length;
        }
    }
    require(index ? length >= 2 && length <= 8 : length >= 2 && length <= 3,
            "invalid locale component");
    return value;
}
struct Resource
{
    std::string record, owner;
    Json::Array layers;
};
struct Translation
{
    std::string text, owner;
    Json::Array history;
};
struct Variant
{
    std::string resource, owner;
    Json when;
    int64_t priority;
};
struct Appearance
{
    std::string fallback, owner;
    std::set<std::string, std::less<>> authors;
    std::vector<Variant> variants;
};
Json scalar_context(const Json &input)
{
    require(input.object().size() <= 64, "resource context field limit");
    Json::Object values;
    for (const auto &[key, encoded] : input.object())
    {
        identifier(key);
        auto value = Value::parse(encoded);
        auto kind = value.type().kind();
        require(kind == "bool" || kind == "i64" || kind == "u64" || kind == "string" ||
                    kind == "name" || kind == "enum",
                "resource context requires scalar values");
        values.emplace(key, value.json());
    }
    return Json(std::move(values));
}
int64_t priority(const Json *encoded)
{
    if (!encoded)
        return 0;
    auto value = Value::parse(Json(Json::Object{{"type", string("i64")}, {"value", *encoded}}));
    int64_t number = 0;
    const auto &source = value.json().at("value").string();
    auto parsed = std::from_chars(source.data(), source.data() + source.size(), number);
    require(parsed.ec == std::errc{} && parsed.ptr == source.data() + source.size(),
            "invalid appearance priority");
    return number;
}
void bounded_nodes(const Json &json, size_t &remaining, size_t depth = 0)
{
    require(remaining && depth <= 64, "expanded resource plan node/depth limit");
    --remaining;
    if (auto array = std::get_if<Json::Array>(&json.value))
        for (const auto &value : *array)
            bounded_nodes(value, remaining, depth + 1);
    if (auto object = std::get_if<Json::Object>(&json.value))
        for (const auto &[key, value] : *object)
        {
            (void)key;
            bounded_nodes(value, remaining, depth + 1);
        }
}
} // namespace
Json compose_resources(const Json &project)
{
    require(to_json(project).size() <= 1048576, "resource project exceeds 1 MiB");
    fields(project, {"format", "types", "packages", "locale", "fallback_locale", "context"});
    require(exact_small_integer(project.at("format"), 1), "unsupported resource project format");
    auto schemas = resolve_schemas(project.at("types"));
    auto context = scalar_context(object_member(project, "context"));
    std::map<std::string, Value, std::less<>> context_values;
    for (const auto &[key, value] : context.object())
        context_values.emplace(key, Value::parse(value));
    auto locale = language(project.at("locale"));
    auto fallback = language(project.at("fallback_locale"));
    const auto &packages = project.at("packages").array();
    require(packages.size() <= 256, "resource package count limit");
    std::vector<Manifest> manifests;
    for (const auto &package : packages)
    {
        fields(package, {"id", "version", "requires", "before", "after", "resources", "patches",
                         "localization", "appearances"});
        // Reuse graph validation only; this internal sentinel is never loaded.
        Json::Object manifest{{"id", package.at("id")},
                              {"name", package.at("id")},
                              {"version", package.at("version")},
                              {"api", Json(1.0)},
                              {"entry", string("resource.dll")}};
        for (auto key : {"requires", "before", "after"})
            if (auto value = package.find(key))
            {
                require(value->array().size() <= 256, "resource dependency/order count limit");
                manifest.emplace(key, *value);
            }
        manifests.push_back(parse_manifest(to_json(Json(manifest))));
    }
    auto order = resolve(manifests, {});
    std::map<std::string, std::set<std::string, std::less<>>, std::less<>> dependencies;
    for (auto index : order)
    {
        auto &closure = dependencies[manifests[index].id];
        for (const auto &dep : manifests[index].requires_mods)
        {
            closure.insert(dep.id);
            const auto &ancestors = dependencies.at(dep.id);
            closure.insert(ancestors.begin(), ancestors.end());
        }
    }
    auto dependency = [&](std::string_view author, std::string_view provider)
    {
        require(author == provider || dependencies.at(std::string(author)).contains(provider),
                std::string(author) + " requires declared dependency on " + std::string(provider));
    };
    Database database(schemas);
    std::map<std::string, Resource, std::less<>> resources;
    std::map<std::string, std::map<std::string, Translation, std::less<>>, std::less<>>
        translations;
    std::map<std::string, Appearance, std::less<>> appearances;
    std::vector<std::pair<Value, std::string>> authored_values;
    size_t translation_count = 0, translation_bytes = 0, variant_count = 0;
    Json::Array sorted;
    for (auto index : order)
    {
        const auto &package = packages[index];
        const auto &owner = manifests[index].id;
        sorted.emplace_back(Json::Object{{"id", string(owner)},
                                         {"version", string(manifests[index].version.str())}});
        try
        {
            auto tx = database.begin(owner, "offline-resource-project");
            for (const auto &[raw_path, definition] : object_member(package, "resources").object())
            {
                auto name = path(string(raw_path));
                require(resources.size() < 4096 && !resources.contains(name),
                        "duplicate resource or resource count limit: " + name);
                fields(definition, {"type", "fields"});
                Resource entry{"resource." + std::to_string(resources.size()), owner, {}};
                tx.create(entry.record, definition.at("type").string());
                Json::Array keys;
                for (const auto &[key, value] : definition.at("fields").object())
                {
                    auto owned = Value::parse(value);
                    authored_values.emplace_back(owned, owner);
                    tx.set(entry.record, key, std::move(owned));
                    keys.emplace_back(key);
                }
                entry.layers.emplace_back(Json::Object{{"owner", string(owner)},
                                                       {"action", string("define")},
                                                       {"fields", Json(keys)}});
                resources.emplace(name, std::move(entry));
            }
            tx.commit();
            const auto &patches = array_member(package, "patches").array();
            require(patches.size() <= 4096, "resource patch count limit");
            for (const auto &patch : patches)
            {
                fields(patch, {"resource", "fields", "expect"});
                auto name = path(patch.at("resource"));
                auto found = resources.find(name);
                require(found != resources.end(), "patch resource missing: " + name);
                auto &entry = found->second;
                dependency(owner, entry.owner);
                auto before = database.snapshot();
                auto write = database.begin(owner, "offline-resource-patch");
                const auto &expect = object_member(patch, "expect").object();
                const auto &edits = patch.at("fields").object();
                require(edits.size() <= 256, "resource patch field limit");
                for (const auto &[key, prior] : expect)
                {
                    require(edits.contains(key) && valid_id(prior.string()),
                            "unused/invalid expected resource owner: " + name + "." + key);
                }
                Json::Array changed;
                for (const auto &[key, encoded] : edits)
                {
                    identifier(key);
                    auto value = Value::parse(encoded);
                    authored_values.emplace_back(value, owner);
                    const Value *old = nullptr;
                    try
                    {
                        old = &before.field(entry.record, key);
                    }
                    catch (const std::invalid_argument &)
                    {
                    }
                    auto prior = expect.find(key);
                    if (old)
                    {
                        auto previous = before.provenance(entry.record, key).back().owner;
                        require(prior == expect.end() || prior->second.string() == previous,
                                "stale resource owner: " + name + "." + key);
                        if (*old == value)
                            continue;
                        require(previous == owner || prior != expect.end(),
                                "resource overlap needs expected owner: " + name + "." + key);
                        dependency(owner, previous);
                    }
                    else
                        require(prior == expect.end(),
                                "expected resource field does not exist: " + name + "." + key);
                    write.set(entry.record, key, std::move(value));
                    changed.emplace_back(key);
                }
                write.commit();
                if (!changed.empty())
                {
                    require(entry.layers.size() < 64, "resource layer history limit");
                    entry.layers.emplace_back(Json::Object{{"owner", string(owner)},
                                                           {"action", string("patch")},
                                                           {"fields", Json(changed)}});
                }
            }
            for (const auto &[raw_locale, table] : object_member(package, "localization").object())
            {
                auto lang = language(string(raw_locale));
                for (const auto &[key, encoded] : table.object())
                {
                    identifier(key);
                    fields(encoded, {"text", "expect_owner"});
                    const auto &value = encoded.at("text").string();
                    text(value, 16384);
                    auto &catalog = translations[lang];
                    auto existing = catalog.find(key);
                    auto expected_owner = encoded.find("expect_owner");
                    if (existing != catalog.end())
                    {
                        require(!expected_owner ||
                                    expected_owner->string() == existing->second.owner,
                                "stale localization owner: " + lang + "." + key);
                        if (existing->second.text == value)
                            continue;
                        require(existing->second.owner == owner || expected_owner,
                                "localization overlap needs expected owner: " + lang + "." + key);
                        dependency(owner, existing->second.owner);
                        require(existing->second.history.size() < 64, "localization history limit");
                        translation_bytes -= existing->second.text.size();
                    }
                    else
                    {
                        require(!expected_owner, "expected localization entry does not exist");
                        require(++translation_count <= 4096, "localization entry limit");
                    }
                    require(value.size() <= 4 * 1048576 - translation_bytes,
                            "localization text byte limit");
                    translation_bytes += value.size();
                    auto history =
                        existing == catalog.end() ? Json::Array{} : existing->second.history;
                    history.emplace_back(owner);
                    catalog.insert_or_assign(key, Translation{value, owner, std::move(history)});
                }
            }
            for (const auto &[name, definition] : object_member(package, "appearances").object())
            {
                identifier(name);
                fields(definition, {"fallback", "variants"});
                auto fallback_path = path(definition.at("fallback"));
                auto [found, inserted] =
                    appearances.try_emplace(name, Appearance{fallback_path, owner, {}, {}});
                require(appearances.size() <= 4096 && found->second.fallback == fallback_path,
                        "appearance fallback conflict: " + name);
                (void)inserted;
                found->second.authors.insert(owner);
                const auto &variants = definition.at("variants").array();
                require(variants.size() <= 256 - found->second.variants.size(),
                        "appearance variant limit");
                for (const auto &variant : variants)
                {
                    fields(variant, {"resource", "when", "priority"});
                    require(++variant_count <= 4096, "total appearance variant limit");
                    found->second.variants.push_back({path(variant.at("resource")), owner,
                                                      scalar_context(variant.at("when")),
                                                      priority(variant.find("priority"))});
                }
            }
        }
        catch (const std::exception &e)
        {
            throw std::invalid_argument("resource package " + owner + ": " + e.what());
        }
    }
    Json::Object all_translations, selected_text;
    std::set<std::string, std::less<>> keys;
    for (const auto &[lang, table] : translations)
    {
        Json::Object output;
        for (const auto &[key, entry] : table)
        {
            keys.insert(key);
            output.emplace(key, Json(Json::Object{{"text", string(entry.text)},
                                                  {"owner", string(entry.owner)},
                                                  {"history", Json(entry.history)}}));
        }
        all_translations.emplace(lang, Json(std::move(output)));
    }
    for (const auto &key : keys)
    {
        require(translations.contains(fallback) && translations.at(fallback).contains(key),
                "localization fallback missing: " + fallback + "." + key);
        const auto &provider = translations.at(fallback).at(key).history.front().string();
        for (const auto &[lang, table] : translations)
        {
            (void)lang;
            auto entry = table.find(key);
            if (entry != table.end())
                for (const auto &author : entry->second.history)
                    dependency(author.string(), provider);
        }
        bool native = translations.contains(locale) && translations.at(locale).contains(key);
        const auto &entry = translations.at(native ? locale : fallback).at(key);
        selected_text.emplace(key, Json(Json::Object{{"text", string(entry.text)},
                                                     {"owner", string(entry.owner)},
                                                     {"locale", string(native ? locale : fallback)},
                                                     {"fallback", Json(!native)}}));
    }
    auto snapshot = database.snapshot();
    auto require_resource = [&](std::string_view name) -> const Resource &
    {
        auto found = resources.find(name);
        require(found != resources.end(), "resource reference missing: " + std::string(name));
        return found->second;
    };
    std::function<void(const Json &, std::string_view)> references;
    references = [&](const Json &value, std::string_view author)
    {
        auto type = value.at("type").string();
        if (type == "array")
            for (const auto &item : value.at("value").array())
                references(item, author);
        if (type == "resource")
            dependency(author, require_resource(path(value.at("value"))).owner);
        if (type == "localization")
        {
            auto key = value.at("value").string();
            require(translations.contains(fallback) && translations.at(fallback).contains(key),
                    "localization reference missing: " + key);
            dependency(author, translations.at(fallback).at(key).history.front().string());
        }
    };
    auto data = snapshot.json();
    for (const auto &[value, author] : authored_values)
        references(value.json(), author);
    Json::Object catalog;
    for (const auto &[name, entry] : resources)
    {
        auto record = data.at("records").at(entry.record).object();
        for (const auto &[key, value] : record.at("fields").object())
            references(value, snapshot.provenance(entry.record, key).back().owner);
        record.emplace("owner", string(entry.owner));
        record.emplace("layers", Json(entry.layers));
        catalog.emplace(name, Json(std::move(record)));
    }
    Json::Object appearance_output;
    for (const auto &[name, group] : appearances)
    {
        const auto &base = require_resource(group.fallback);
        for (const auto &author : group.authors)
            dependency(author, base.owner);
        bool matched = false, ambiguous = false;
        int64_t best = 0;
        std::string chosen = group.fallback;
        Json::Array winners, variants;
        for (const auto &variant : group.variants)
        {
            const auto &target = require_resource(variant.resource);
            dependency(variant.owner, target.owner);
            require(snapshot.record_type(target.record) == snapshot.record_type(base.record),
                    "appearance resource type mismatch: " + name);
            variants.emplace_back(
                Json::Object{{"owner", string(variant.owner)},
                             {"resource", string(variant.resource)},
                             {"when", variant.when},
                             {"priority", string(std::to_string(variant.priority))}});
            bool matches = true;
            for (const auto &[key, value] : variant.when.object())
            {
                auto actual = context_values.find(key);
                if (actual == context_values.end() || !(actual->second == Value::parse(value)))
                    matches = false;
            }
            if (!matches || (matched && variant.priority < best))
                continue;
            if (!matched || variant.priority > best)
            {
                winners.clear();
                ambiguous = false;
                chosen = variant.resource;
            }
            else if (chosen != variant.resource)
                ambiguous = true;
            best = variant.priority;
            matched = true;
            winners.emplace_back(variant.owner);
        }
        require(!ambiguous, "ambiguous appearance priority tie: " + name);
        if (!matched)
            winners.emplace_back(group.owner);
        appearance_output.emplace(
            name,
            Json(Json::Object{
                {"fallback", string(group.fallback)},
                {"variants", Json(variants)},
                {"selected", Json(Json::Object{{"resource", string(chosen)},
                                               {"owners", Json(winners)},
                                               {"fallback", Json(!matched)},
                                               {"priority", matched ? string(std::to_string(best))
                                                                    : Json(nullptr)}})}}));
    }
    Json::Object output{{"format", Json(1.0)},
                        {"mode", string("offline-resource-plan")},
                        {"types", schemas},
                        {"context", context},
                        {"locale", string(locale)},
                        {"fallback_locale", string(fallback)},
                        {"order", Json(sorted)},
                        {"resources", Json(catalog)},
                        {"localization", Json(Json::Object{{"translations", Json(all_translations)},
                                                           {"selected", Json(selected_text)}})},
                        {"appearances", Json(appearance_output)},
                        {"engine_adapter_ready", Json(false)},
                        {"game_tested", Json(false)}};
    Json result(std::move(output));
    size_t nodes = 262143; // Reserve the future cache-key value node.
    bounded_nodes(result, nodes);
    auto encoded = to_json(result);
    require(encoded.size() <= 16 * 1048576, "expanded resource plan byte limit");
    std::get<Json::Object>(result.value).emplace("cache_key", string(resource_digest(encoded)));
    require(to_json(result).size() <= 16 * 1048576, "expanded resource plan byte limit");
    return result;
}
} // namespace nf::resources
