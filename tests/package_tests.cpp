#include "manifest.hpp"
#include "test.hpp"
#include <algorithm>
using namespace nf;
Manifest mod(std::string id)
{
    Manifest m;
    m.id = id;
    m.name = id;
    m.entry = "plugin.dll";
    m.version = {1, 0, 0};
    return m;
}
TEST(json_values_and_unicode)
{
    auto j = parse_json(R"({"a":[null,true,-125.5e2],"text":"\uD83D\uDE80"})");
    CHECK(j.at("a").array().size() == 3);
    CHECK(j.at("a").array()[2].number() == -12550);
    CHECK(j.at("text").string() == "\xF0\x9F\x9A\x80");
}
TEST(json_roundtrip)
{
    auto j = parse_json(R"({"escaped":"a\nb\t\"c\\d","x":0.001,"f":false})");
    CHECK(to_json(parse_json(to_json(j))) == to_json(j));
}
TEST(json_rejects_bad_syntax)
{
    for (auto s : {"", "[1,]", "{\"a\":1,}", "true false", "01", "+1", "1.", "1e", "NaN",
                   "{\"a\":1,\"a\":2}", "\"\\uD800\"", "\"\\uDC00\""})
        rejects<ParseError>([&] { parse_json(s); });
}
TEST(json_rejects_invalid_utf8)
{
    std::string s = "\"\xC0\xAF\"";
    rejects<ParseError>([&] { parse_json(s); });
    CHECK(!valid_utf8("\xED\xA0\x80"));
    CHECK(valid_utf8("Night City \xE2\x9C\x93"));
}
TEST(json_depth_and_nonfinite_limits)
{
    rejects<ParseError>([] { parse_json(std::string(80, '[') + "0" + std::string(80, ']')); });
    rejects<ParseError>([] { parse_json("1e999"); });
}
TEST(json_error_has_position)
{
    try
    {
        parse_json("[true, nope]");
        CHECK(false);
    }
    catch (const ParseError &e)
    {
        CHECK(e.offset >= 7);
    }
}
TEST(version_strict_and_ordered)
{
    CHECK(Version::parse("2.10.1") > Version::parse("2.9.99"));
    CHECK(Version::parse("0.1.0").str() == "0.1.0");
    for (auto s : {"1", "1.2", "01.2.3", "-1.0.0", "1.2.3.4", "4294967296.0.0"})
        rejects([&] { Version::parse(s); });
}
TEST(ids_and_windows_paths)
{
    CHECK(valid_id("com.example.mod-a"));
    for (auto s : {"single", "Com.example", "com..bad", "com.example/evil", "com.example."})
        CHECK(!valid_id(s));
    CHECK(safe_relative_path("native/plugin.dll"));
    for (auto s : {"../evil.dll", "C:/evil.dll", "/evil.dll", "a/../b.dll", "a\\..\\b.dll",
                   "native/a.dll:stream", "CON", "a. /b.dll", "a//b.dll"})
        CHECK(!safe_relative_path(s));
}
TEST(manifest_schema)
{
    auto m = parse_manifest(
        R"({"id":"com.example.mod","name":"Example","version":"1.2.0","api":1,"entry":"native/a.dll","capabilities":[{"name":"nf.events","version":1,"required":true}]})");
    CHECK(m.version.minor == 2);
    CHECK(m.capabilities.size() == 1);
}
TEST(manifest_rejects_typo_types_and_escape)
{
    for (
        auto s :
        {R"({"id":"com.example.mod","name":"A","version":"1.0.0","api":1,"entry":"a.dll","entyr":"b.dll"})",
         R"({"id":"com.example.mod","name":"A","version":"1.0.0","api":1.5,"entry":"a.dll"})",
         R"({"id":"com.example.mod","name":"A","version":"1.0.0","api":1,"entry":"../a.dll"})"})
        rejects([&] { parse_manifest(s); });
}
TEST(graph_order_stable)
{
    auto a = mod("com.test.a"), b = mod("com.test.b"), c = mod("com.test.c");
    b.requires_mods.push_back({a.id, {1, 0, 0}, Version{2, 0, 0}});
    std::vector<Manifest> ms{c, b, a};
    auto order = resolve(ms, {});
    CHECK(ms[order[0]].id == a.id);
    CHECK(ms[order[1]].id == b.id);
    CHECK(ms[order[2]].id == c.id);
}
TEST(graph_rejects_missing_and_versions)
{
    auto a = mod("com.test.a"), b = mod("com.test.b");
    b.requires_mods.push_back({a.id, {2, 0, 0}, {}});
    rejects([&] { resolve({b}, {}); });
    rejects([&] { resolve({a, b}, {}); });
}
TEST(graph_rejects_duplicates_cycles_and_self)
{
    auto a = mod("com.test.a"), b = mod("com.test.b");
    rejects([&] { resolve({a, a}, {}); });
    a.after.push_back(b.id);
    b.after.push_back(a.id);
    rejects([&] { resolve({a, b}, {}); });
    a.after = {a.id};
    rejects([&] { resolve({a}, {}); });
}
TEST(capability_preflight)
{
    auto a = mod("com.test.a");
    a.capabilities = {{"nf.game.world", 1, true}};
    rejects([&] { resolve({a}, {{"nf.events", 1}}); });
    a.capabilities[0].required = false;
    CHECK(resolve({a}, {}).size() == 1);
    a.capabilities = {{"nf.events", 2, true}};
    rejects([&] { resolve({a}, {{"nf.events", 1}}); });
}
int main() { return run_tests(); }
