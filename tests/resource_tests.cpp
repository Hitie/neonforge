#include "resource_model.hpp"
#include "test.hpp"
#include <thread>
using namespace nf;
using namespace nf::resources;
Json fixture()
{
    return parse_json_exact(
        R"({"format":1,"locale":"fr-fr","fallback_locale":"en-us","context":{"body":{"type":"name","value":"Slim"}},"types":{"Entity":{"fields":{"title":{"type":"localization","required":true},"mesh":{"type":"resource"},"enabled":{"type":"bool"}}},"Visual":{"fields":{"label":{"type":"string","required":true}}}},"packages":[{"id":"example.base","version":"1.0.0","resources":{"original/entity.nf":{"type":"Entity","fields":{"title":{"type":"localization","value":"Greeting"},"mesh":{"type":"resource","value":"original/mesh.nf"}}},"original/mesh.nf":{"type":"Visual","fields":{"label":{"type":"string","value":"Original"}}},"original/slim.nf":{"type":"Visual","fields":{"label":{"type":"string","value":"Slim"}}}},"localization":{"en-us":{"Greeting":{"text":"Hello"}},"ja-jp":{"Greeting":{"text":"こんにちは"}}},"appearances":{"original.body":{"fallback":"original/mesh.nf","variants":[{"resource":"original/slim.nf","when":{"body":{"type":"name","value":"Slim"}},"priority":"10"}]}}},{"id":"example.patch","version":"1.0.0","requires":[{"id":"example.base","min":"1.0.0"}],"patches":[{"resource":"original/entity.nf","fields":{"enabled":{"type":"bool","value":true},"mesh":{"type":"resource","value":"original/slim.nf"}},"expect":{"mesh":"example.base"}}],"localization":{"en-us":{"Greeting":{"text":"Welcome","expect_owner":"example.base"}}}}]})");
}
Json &part(Json &value, std::initializer_list<std::string_view> keys)
{
    auto *out = &value;
    for (auto key : keys)
        out = &std::get<Json::Object>(out->value).at(std::string(key));
    return *out;
}
Json &package(Json &project, size_t index)
{
    return std::get<Json::Array>(part(project, {"packages"}).value).at(index);
}
TEST(resource_typed_composition_localization_and_appearance)
{
    auto result = compose_resources(fixture());
    CHECK(result.at("order").array()[0].at("id").string() == "example.base");
    CHECK(result.at("resources").object().size() == 3);
    CHECK(result.at("resources")
              .at("original/entity.nf")
              .at("fields")
              .at("enabled")
              .at("value")
              .boolean());
    CHECK(result.at("localization").at("selected").at("Greeting").at("text").string() == "Welcome");
    CHECK(result.at("localization").at("selected").at("Greeting").at("fallback").boolean());
    CHECK(result.at("appearances").at("original.body").at("selected").at("resource").string() ==
          "original/slim.nf");
    CHECK(!result.at("engine_adapter_ready").boolean() && !result.at("game_tested").boolean());
}
TEST(resource_dependency_order_is_deterministic_and_versions_are_checked)
{
    auto project = fixture();
    auto first = to_json(compose_resources(project));
    auto &packages = std::get<Json::Array>(part(project, {"packages"}).value);
    std::swap(packages[0], packages[1]);
    CHECK(to_json(compose_resources(project)) == first);
    for (auto dependency_spec :
         {R"([{"id":"missing.package","min":"1.0.0"}])", R"([{"id":"example.base","min":"2.0.0"}])",
          R"([{"id":"example.base","min":"0.0.0","max_exclusive":"1.0.0"}])"})
    {
        auto bad = fixture();
        part(package(bad, 1), {"requires"}) = parse_json(dependency_spec);
        rejects([&] { compose_resources(bad); });
    }
    auto cycle = fixture();
    std::get<Json::Object>(package(cycle, 0).value)["requires"] =
        parse_json(R"([{"id":"example.patch","min":"1.0.0"}])");
    rejects([&] { compose_resources(cycle); });
}
TEST(resource_overlaps_require_current_owners_and_types)
{
    for (int mode = 0; mode < 5; ++mode)
    {
        auto bad = fixture();
        auto &patch = std::get<Json::Array>(part(package(bad, 1), {"patches"}).value)[0];
        if (mode == 0)
            part(patch, {"expect"}) = Json(Json::Object{});
        if (mode == 1)
            part(patch, {"expect", "mesh"}) = Json(std::string("example.stale"));
        if (mode == 2)
            std::get<Json::Object>(part(patch, {"expect"}).value)["unused"] =
                Json(std::string("example.base"));
        if (mode == 3)
            part(patch, {"fields", "enabled"}) = parse_json(R"({"type":"u64","value":"1"})");
        if (mode == 4)
            part(package(bad, 1), {"requires"}) = Json(Json::Array{});
        rejects([&] { compose_resources(bad); });
    }
    auto duplicate = fixture();
    std::get<Json::Object>(package(duplicate, 1).value)["resources"] =
        part(package(duplicate, 0), {"resources"});
    rejects([&] { compose_resources(duplicate); });
    auto required = fixture();
    part(package(required, 0), {"resources", "original/mesh.nf", "fields"}) = Json(Json::Object{});
    rejects([&] { compose_resources(required); });
}
TEST(resource_references_and_unused_appearance_rules_are_validated)
{
    for (auto path :
         {"../escape.nf", "Original/mesh.nf", "original\\mesh.nf", "original/missing.nf"})
    {
        auto bad = fixture();
        part(package(bad, 0), {"resources", "original/entity.nf", "fields", "mesh", "value"}) =
            Json(std::string(path));
        rejects([&] { compose_resources(bad); });
    }
    auto bad = fixture();
    auto &variant = std::get<Json::Array>(
        part(package(bad, 0), {"appearances", "original.body", "variants"}).value)[0];
    part(variant, {"resource"}) = Json(std::string("original/missing.nf"));
    part(bad, {"context"}) = Json(Json::Object{});
    rejects([&] { compose_resources(bad); });
    auto missing = fixture();
    part(package(missing, 0), {"resources", "original/entity.nf", "fields", "title", "value"}) =
        Json(std::string("Missing"));
    rejects([&] { compose_resources(missing); });
}
TEST(resource_localization_fallback_and_overlap_are_explicit)
{
    auto japanese = fixture();
    part(japanese, {"locale"}) = Json(std::string("ja-jp"));
    CHECK(compose_resources(japanese)
              .at("localization")
              .at("selected")
              .at("Greeting")
              .at("text")
              .string() == "こんにちは");
    auto bad = fixture();
    std::get<Json::Object>(part(package(bad, 1), {"localization", "en-us", "Greeting"}).value)
        .erase("expect_owner");
    rejects([&] { compose_resources(bad); });
    auto nofallback = fixture();
    part(nofallback, {"fallback_locale"}) = Json(std::string("de-de"));
    rejects([&] { compose_resources(nofallback); });
}
TEST(resource_appearance_priority_ties_and_fallback_are_checked)
{
    auto project = fixture();
    part(project, {"context"}) = Json(Json::Object{});
    CHECK(compose_resources(project)
              .at("appearances")
              .at("original.body")
              .at("selected")
              .at("resource")
              .string() == "original/mesh.nf");
    auto tied = fixture();
    auto &variants = std::get<Json::Array>(
        part(package(tied, 0), {"appearances", "original.body", "variants"}).value);
    auto variant = variants[0];
    part(variant, {"resource"}) = Json(std::string("original/mesh.nf"));
    variants.push_back(variant);
    rejects([&] { compose_resources(tied); });
    part(variants[0], {"priority"}) = Json(std::string("9007199254740993"));
    part(variants[1], {"priority"}) = Json(std::string("9007199254740992"));
    CHECK(compose_resources(tied)
              .at("appearances")
              .at("original.body")
              .at("selected")
              .at("resource")
              .string() == "original/slim.nf");
}
TEST(resource_only_ties_at_final_highest_priority_are_ambiguous)
{
    auto project = fixture();
    auto &variants = std::get<Json::Array>(
        part(package(project, 0), {"appearances", "original.body", "variants"}).value);
    auto first = variants[0];
    part(first, {"priority"}) = Json(std::string("0"));
    variants[0] = first;
    auto second = first;
    part(second, {"resource"}) = Json(std::string("original/mesh.nf"));
    variants.push_back(second);
    auto winner = first;
    part(winner, {"priority"}) = Json(std::string("10"));
    variants.push_back(winner);
    CHECK(compose_resources(project)
              .at("appearances")
              .at("original.body")
              .at("selected")
              .at("resource")
              .string() == "original/slim.nf");
}
TEST(resource_sha256_and_last_valid_immutable_session)
{
    CHECK(resource_digest("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(resource_digest(std::string_view{}) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    ResourceSession session;
    rejects([&] { session.snapshot(); });
    auto project = fixture();
    CHECK(session.reload(to_json(project)));
    CHECK(session.generation() == 1);
    auto old = session.snapshot();
    auto before = to_json(*old);
    CHECK(session.reload(to_json(project)));
    CHECK(session.generation() == 1);
    CHECK(!session.reload("{"));
    CHECK(!session.error().empty());
    CHECK(to_json(*session.snapshot()) == before);
    part(project, {"context"}) = Json(Json::Object{});
    CHECK(session.reload(to_json(project)));
    CHECK(session.generation() == 2);
    CHECK(to_json(*old) == before);
    CHECK(session.error().empty());
    bool rejected = false;
    std::thread other(
        [&]
        {
            try
            {
                session.reload(to_json(project));
            }
            catch (...)
            {
                rejected = true;
            }
        });
    other.join();
    CHECK(rejected);
}
TEST(resource_transitive_dependencies_and_nested_references)
{
    auto project = fixture();
    auto consumer = parse_json(
        R"({"id":"example.consumer","version":"1.0.0","requires":[{"id":"example.patch","min":"1.0.0"}],"resources":{"original/consumer.nf":{"type":"Entity","fields":{"title":{"type":"localization","value":"Greeting"},"mesh":{"type":"resource","value":"original/mesh.nf"}}}}})");
    std::get<Json::Array>(part(project, {"packages"}).value).push_back(consumer);
    CHECK(compose_resources(project).at("resources").object().size() == 4);
    part(package(project, 2), {"requires"}) = Json(Json::Array{});
    rejects([&] { compose_resources(project); });
    auto nested = fixture();
    std::get<Json::Object>(part(nested, {"types", "Entity", "fields"}).value)["links"] =
        parse_json(R"({"type":"array","element":{"type":"resource"}})");
    auto &patch = std::get<Json::Array>(part(package(nested, 1), {"patches"}).value)[0];
    std::get<Json::Object>(part(patch, {"fields"}).value)["links"] = parse_json(
        R"({"type":"array","element":{"type":"resource"},"value":[{"type":"resource","value":"original/missing.nf"}]})");
    rejects([&] { compose_resources(nested); });
}
TEST(resource_schema_inheritance_and_noop_attribution)
{
    auto project = fixture();
    auto &types = std::get<Json::Object>(part(project, {"types"}).value);
    types["Base"] = parse_json(R"({"fields":{"title":{"type":"localization","required":true}}})");
    auto &entity = std::get<Json::Object>(types.at("Entity").value);
    std::get<Json::Object>(entity.at("fields").value).erase("title");
    entity.emplace("extends", Json(std::string("Base")));
    auto &patch = std::get<Json::Array>(part(package(project, 1), {"patches"}).value)[0];
    part(patch, {"fields", "mesh", "value"}) = Json(std::string("original/mesh.nf"));
    auto plan = compose_resources(project);
    const auto &history =
        plan.at("resources").at("original/entity.nf").at("provenance").at("mesh").array();
    CHECK(history.size() == 1 && history[0].at("owner").string() == "example.base");
    CHECK(plan.at("types").at("Entity").at("fields").at("title").at("required").boolean());
}
TEST(resource_limits_and_failed_late_reload_preserve_readers)
{
    ResourceSession session;
    CHECK(session.reload(to_json(fixture())));
    auto old = session.snapshot();
    auto bad = fixture();
    part(bad, {"fallback_locale"}) = Json(std::string("zz-zz"));
    CHECK(!session.reload(to_json(bad)));
    CHECK(session.snapshot() == old && session.generation() == 1);
    CHECK(!session.reload(std::string(1048577, ' ')));
    CHECK(session.snapshot() == old);
    auto padded = fixture();
    part(padded, {"context"}) = Json(Json::Object{});
    CHECK(!session.reload(to_json(padded) + std::string(1048577, ' ')));
    CHECK(session.snapshot() == old && session.generation() == 1);
    CHECK(session.error().find("1 MiB") != std::string_view::npos);
    auto too_many = fixture();
    Json::Array packages;
    for (int i = 0; i < 257; ++i)
        packages.emplace_back(Json::Object{{"id", Json("example.p" + std::to_string(i))},
                                           {"version", Json(std::string("1.0.0"))}});
    part(too_many, {"packages"}) = Json(packages);
    rejects([&] { compose_resources(too_many); });
}
int main() { return run_tests(); }
