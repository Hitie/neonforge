#include "data_model.hpp"
#include "test.hpp"
#include <limits>
#include <thread>
using namespace nf;
using namespace nf::data;
Value val(std::string_view json) { return Value::parse(parse_json(json)); }
Json schemas()
{
    return parse_json(
        R"({"Item":{"fields":{"damage":{"type":"u64","required":true},"title":{"type":"string"},"tags":{"type":"array","element":{"type":"name"}}}}})");
}
void seed(Database &db)
{
    auto tx = db.begin("example.base", "base fixture");
    tx.create("Items.Base", "Item");
    tx.set("Items.Base", "damage", val(R"({"type":"u64","value":"18446744073709551615"})"));
    tx.set("Items.Base", "tags", val(R"({"type":"array","element":{"type":"name"},"value":[]})"));
    CHECK(tx.commit() == 1);
}
TEST(data_values_preserve_full_width_integers)
{
    auto n = val(R"({"type":"u64","value":"18446744073709551615"})");
    CHECK(n.json().at("value").string() == "18446744073709551615");
    auto signed_value = val(R"({"type":"i64","value":"-9223372036854775808"})");
    CHECK(signed_value.json().at("value").string() == "-9223372036854775808");
    for (auto bad : {R"({"type":"u64","value":18446744073709551615})",
                     R"({"type":"u64","value":"18446744073709551616"})",
                     R"({"type":"i64","value":"-9223372036854775809"})",
                     R"({"type":"u64","value":"01"})", R"({"type":"i64","value":"-0"})"})
        rejects([&] { val(bad); });
}
TEST(data_compound_values_and_array_domains_are_checked)
{
    CHECK(val(R"({"type":"vector3","value":[1,2,3]})").json().at("value").array()[2].number() == 3);
    CHECK(val(R"({"type":"color","value":[0,128,255,255]})").type().kind() == "color");
    CHECK(val(R"({"type":"array","element":{"type":"enum","enum_type":"Rarity"},"value":[]})")
              .type()
              .element()
              .kind() == "enum");
    for (
        auto bad :
        {R"({"type":"vector3","value":[1,2]})", R"({"type":"color","value":[0,1,2,256]})",
         R"({"type":"array","element":{"type":"u64"},"value":[{"type":"i64","value":"1"}]})",
         R"({"type":"array","element":{"type":"enum","enum_type":"A"},"value":[{"type":"enum","enum_type":"B","value":"One"}]})",
         R"({"type":"u64","value":"1","typo":true})"})
        rejects([&] { val(bad); });
    Json::Object nan{{"type", Json(std::string("f64"))},
                     {"value", Json(std::numeric_limits<double>::infinity())}};
    rejects([&] { Value::parse(Json(nan)); });
}
TEST(data_values_copy_owned_input_and_bound_nested_arrays)
{
    auto source = parse_json(R"({"type":"string","value":"original"})");
    auto value = Value::parse(source);
    source = Json(nullptr);
    CHECK(value.json().at("value").string() == "original");
    Json descriptor = parse_json(R"({"type":"u64"})");
    for (int i = 0; i < 17; ++i)
        descriptor =
            Json(Json::Object{{"type", Json(std::string("array"))}, {"element", descriptor}});
    rejects([&] { Type::parse(descriptor); });
    rejects([&] { val(R"({"type":"resource","value":"../escape.mesh"})"); });
}
TEST(data_snapshot_does_not_change_until_atomic_commit)
{
    Database db(schemas());
    seed(db);
    auto old = db.snapshot();
    auto tx = db.begin("example.edit");
    tx.set("Items.Base", "damage", val(R"({"type":"u64","value":"42"})"));
    CHECK(db.snapshot().field("Items.Base", "damage").json().at("value").string() ==
          "18446744073709551615");
    CHECK(tx.commit() == 2);
    CHECK(old.revision() == 1);
    CHECK(old.field("Items.Base", "damage").json().at("value").string() == "18446744073709551615");
    CHECK(db.snapshot().field("Items.Base", "damage").json().at("value").string() == "42");
    rejects([&] { tx.erase_record("Items.Base"); });
}
TEST(data_caught_failure_poison_and_abort_preserve_state)
{
    Database db(schemas());
    seed(db);
    auto tx = db.begin("example.edit");
    tx.set("Items.Base", "damage", val(R"({"type":"u64","value":"42"})"));
    rejects([&] { tx.set("Items.Base", "damage", val(R"({"type":"i64","value":"42"})")); });
    rejects([&] { tx.commit(); });
    CHECK(db.snapshot().revision() == 1);
    auto aborted = db.begin("example.edit");
    aborted.erase_record("Items.Base");
    aborted.abort();
    rejects([&] { aborted.commit(); });
    CHECK(db.snapshot().contains("Items.Base"));
}
TEST(data_stale_and_required_field_failures_do_not_publish)
{
    Database db(schemas());
    seed(db);
    auto a = db.begin("example.first"), b = db.begin("example.second");
    a.set("Items.Base", "damage", val(R"({"type":"u64","value":"4"})"));
    CHECK(a.commit() == 2);
    b.erase_record("Items.Base");
    rejects([&] { b.commit(); });
    CHECK(db.snapshot().contains("Items.Base"));
    auto incomplete = db.begin("example.first");
    incomplete.create("Items.Missing", "Item");
    rejects([&] { incomplete.commit(); });
    auto missing = db.begin("example.first");
    missing.erase_field("Items.Base", "damage");
    rejects([&] { missing.commit(); });
    CHECK(db.snapshot().revision() == 2);
}
TEST(data_clone_arrays_and_owner_history_are_independent)
{
    Database db(schemas());
    seed(db);
    auto tx = db.begin("example.edit", "patch.json");
    tx.clone("Items.Copy", "Items.Base");
    auto tag = val(R"({"type":"name","value":"Custom"})");
    CHECK(tx.append("Items.Copy", "tags", tag, true));
    CHECK(!tx.append("Items.Copy", "tags", tag, true));
    CHECK(tx.append("Items.Copy", "tags", val(R"({"type":"name","value":"First"})"), false, true));
    CHECK(tx.commit() == 2);
    CHECK(db.snapshot().field("Items.Copy", "tags").json().at("value").array().size() == 2);
    CHECK(db.snapshot().field("Items.Base", "tags").json().at("value").array().empty());
    auto history = db.snapshot().provenance("Items.Copy", "damage");
    CHECK(history.size() == 2 && history[0].owner == "example.base" &&
          history[1].operation == "clone");
    auto remove = db.begin("example.edit");
    CHECK(remove.remove("Items.Copy", "tags", tag) == 1);
    CHECK(remove.commit() == 3);
    CHECK(db.snapshot().field("Items.Copy", "tags").json().at("value").array().size() == 1);
    auto noop = db.begin("example.edit");
    CHECK(noop.remove("Items.Copy", "tags", tag) == 0);
    CHECK(noop.commit() == 3);
}
TEST(data_budget_failure_is_atomic)
{
    Database db(schemas(), Limits{1, 8, 4096, 32, 8});
    seed(db);
    auto tx = db.begin("example.edit");
    tx.clone("Items.Extra", "Items.Base");
    rejects([&] { tx.commit(); });
    CHECK(db.snapshot().record_count() == 1 && db.snapshot().revision() == 1);
    Database history_db(schemas(), Limits{4, 8, 4096, 32, 1});
    seed(history_db);
    auto edit = history_db.begin("example.edit");
    rejects([&] { edit.set("Items.Base", "damage", val(R"({"type":"u64","value":"42"})")); });
    rejects([&] { edit.commit(); });
}
TEST(data_expired_database_and_wrong_thread_are_rejected)
{
    auto db = std::make_unique<Database>(schemas());
    seed(*db);
    auto snapshot = db->snapshot();
    auto tx = db->begin("example.edit");
    db.reset();
    rejects([&] { tx.commit(); });
    CHECK(snapshot.contains("Items.Base"));
    Database active(schemas());
    bool rejected = false;
    std::thread other(
        [&]
        {
            try
            {
                active.begin("example.edit");
            }
            catch (const std::exception &)
            {
                rejected = true;
            }
        });
    other.join();
    CHECK(rejected);
}
TEST(data_failed_transactions_can_abort_and_moved_from_handles_are_safe)
{
    Database db(schemas());
    seed(db);
    auto tx = db.begin("example.edit");
    rejects([&] { tx.erase_record("Items.Missing"); });
    tx.abort();
    rejects([&] { tx.commit(); });
    auto original = db.begin("example.edit");
    auto moved = std::move(original);
    rejects([&] { original.commit(); });
    moved.abort();
    CHECK(db.snapshot().revision() == 1);
}
TEST(data_provenance_is_included_in_the_state_byte_budget)
{
    Database db(schemas(), Limits{4, 8, 1500, 64, 64});
    seed(db);
    auto initial = db.snapshot();
    auto tx = db.begin("example.edit", std::string(240, 'x'));
    for (int i = 0; i < 6; ++i)
        tx.set("Items.Base", "damage", val(R"({"type":"u64","value":"42"})"));
    rejects([&] { tx.commit(); });
    CHECK(db.snapshot().revision() == initial.revision());
    CHECK(db.snapshot().field("Items.Base", "damage") == initial.field("Items.Base", "damage"));
}
TEST(data_moved_values_and_snapshots_cannot_dereference_empty_storage)
{
    auto first = val(R"({"type":"u64","value":"4"})");
    auto moved = std::move(first);
    rejects([&] { first.json(); });
    CHECK(moved.json().at("value").string() == "4");
    Database db(schemas());
    seed(db);
    auto old = db.snapshot();
    auto current = std::move(old);
    rejects([&] { old.revision(); });
    CHECK(current.revision() == 1);
}
TEST(data_values_reject_invalid_text_and_oversized_arrays)
{
    Json bad(Json::Object{{"type", Json(std::string("string"))},
                          {"value", Json(std::string("\xc0\xaf", 2))}});
    rejects([&] { Value::parse(bad); });
    Json::Array values(4097, parse_json(R"({"type":"bool","value":true})"));
    Json array(Json::Object{{"type", Json(std::string("array"))},
                            {"element", parse_json(R"({"type":"bool"})")},
                            {"value", Json(values)}});
    rejects([&] { Value::parse(array); });
    rejects(
        [&]
        {
            Database missing_schema(parse_json(R"({"Item":{"fields":{"x":{"type":"unknown"}}}})"));
        });
}
TEST(data_clean_and_net_zero_transactions_do_not_spend_budget_or_revision)
{
    Database tiny(parse_json("{}"), Limits{1, 1, 1, 8, 8});
    auto clean = tiny.begin("example.noop");
    CHECK(clean.commit() == 0);
    Database db(schemas());
    auto cancelled = db.begin("example.edit");
    cancelled.create("Items.Temp", "Item");
    cancelled.erase_record("Items.Temp");
    CHECK(cancelled.commit() == 0);
    seed(db);
    auto temporary = db.begin("example.edit");
    temporary.set("Items.Base", "title", val(R"({"type":"string","value":"temporary"})"));
    temporary.erase_field("Items.Base", "title");
    CHECK(temporary.commit() == 1);
}
TEST(data_cross_thread_failure_poisons_staged_edits)
{
    Database db(schemas());
    seed(db);
    auto tx = db.begin("example.edit");
    tx.erase_record("Items.Base");
    bool rejected = false;
    std::thread other(
        [&]
        {
            try
            {
                tx.create("Items.Other", "Item");
            }
            catch (const std::exception &)
            {
                rejected = true;
            }
        });
    other.join();
    CHECK(rejected);
    rejects([&] { tx.commit(); });
    CHECK(db.snapshot().contains("Items.Base"));
}
TEST(data_value_parse_failure_reports_record_and_field)
{
    auto project = parse_json(
        R"({"format":1,"schemas":{"Item":{"fields":{"damage":{"type":"u64"}}}},"transactions":[{"owner":"example.edit","operations":[{"op":"create","record":"Items.Bad","type":"Item"},{"op":"set","record":"Items.Bad","field":"damage","value":{"type":"u64","value":"01"}}]}]})");
    bool contextual = false;
    try
    {
        apply_project(project);
    }
    catch (const std::exception &e)
    {
        std::string message = e.what();
        contextual =
            message.find("Items.Bad") != message.npos && message.find("damage") != message.npos;
    }
    CHECK(contextual);
}
TEST(data_expanded_plan_has_an_independent_report_limit)
{
    Json::Object fields;
    for (int i = 0; i < 32; ++i)
        fields.emplace("f" + std::to_string(i), parse_json(R"({"type":"u64"})"));
    Json::Object schema{{"Item", Json(Json::Object{{"fields", Json(fields)}})}};
    Json::Array setup{parse_json(R"({"op":"create","record":"Base","type":"Item"})")};
    for (int i = 0; i < 32; ++i)
        setup.emplace_back(Json::Object{{"op", Json(std::string("set"))},
                                        {"record", Json(std::string("Base"))},
                                        {"field", Json("f" + std::to_string(i))},
                                        {"value", parse_json(R"({"type":"u64","value":"1"})")}});
    setup.push_back(parse_json(R"({"op":"clone","record":"Target","from":"Base"})"));
    Json::Array batches{Json(
        Json::Object{{"owner", Json(std::string("example.base"))}, {"operations", Json(setup)}})};
    for (int i = 0; i < 150; ++i)
        batches.emplace_back(Json::Object{
            {"owner", Json(std::string(i % 2 ? "example.a" : "example.b"))},
            {"operations",
             parse_json(
                 R"([{"op":"erase_record","record":"Target"},{"op":"clone","record":"Target","from":"Base"}])")}});
    Json project(Json::Object{
        {"format", Json(1.0)}, {"schemas", Json(schema)}, {"transactions", Json(batches)}});
    CHECK(apply_project(project).at("revision").string() == "151");
    bool bounded = false;
    try
    {
        apply_project(project, true);
    }
    catch (const std::exception &e)
    {
        bounded = std::string(e.what()).find("plan") != std::string::npos;
    }
    CHECK(bounded);
}
int main() { return run_tests(); }
