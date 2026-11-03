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
TEST(data_schema_inheritance_is_additive_and_validates_unused_definitions)
{
    auto definitions = parse_json(
        R"({"Child":{"extends":"Base","fields":{"label":{"type":"string"}}},"Base":{"fields":{"id":{"type":"u64","required":true}}}})");
    Database db(definitions);
    auto tx = db.begin("example.inherit");
    tx.create("A", "Child");
    rejects([&] { tx.commit(); });
    auto valid = db.begin("example.inherit");
    valid.create("A", "Child");
    valid.set("A", "id", val(R"({"type":"u64","value":"18446744073709551615"})"));
    CHECK(valid.commit() == 1);
    for (
        auto bad :
        {R"({"A":{"extends":"B","fields":{}},"B":{"extends":"A","fields":{}}})",
         R"({"A":{"extends":"Missing","fields":{}}})",
         R"({"Base":{"fields":{"x":{"type":"u64","required":true}}},"Child":{"extends":"Base","fields":{"x":{"type":"u64"}}}})"})
        rejects([&] { Database invalid(parse_json(bad)); });
    Json::Object chain;
    for (int i = 0; i < 34; ++i)
    {
        Json::Object schema{{"fields", Json(Json::Object{})}};
        if (i)
            schema.emplace("extends", Json(std::to_string(i - 1)));
        chain.emplace(std::to_string(i), Json(schema));
    }
    rejects([&] { Database too_deep{Json(chain)}; });
}
TEST(data_templates_inherit_typed_values_and_instances_are_independent)
{
    auto project = parse_json_exact(
        R"({"format":1,"schemas":{"Base":{"fields":{"id":{"type":"u64","required":true}}},"Item":{"extends":"Base","fields":{"label":{"type":"string"}}}},"templates":{"Base":{"type":"Item","fields":{"id":{"type":"u64","value":"18446744073709551615"}}},"Child":{"extends":"Base","fields":{"label":{"type":"string","value":"child"}}}},"transactions":[{"owner":"example.templates","operations":[{"op":"instantiate","record":"One","template":"Child","fields":{"label":{"type":"string","value":"override"}}},{"op":"instantiate","record":"Two","template":"Child"}]}]})");
    auto result = apply_project(project);
    CHECK(result.at("records").at("One").at("fields").at("id").at("value").string() ==
          "18446744073709551615");
    CHECK(result.at("records").at("One").at("fields").at("label").at("value").string() ==
          "override");
    CHECK(result.at("records").at("Two").at("fields").at("label").at("value").string() == "child");
    CHECK(apply_project(project, true).at("transactions").array().size() == 1);
    auto bad = project.object();
    bad["templates"] =
        parse_json(R"({"Unused":{"type":"Item","fields":{"id":{"type":"i64","value":"1"}}}})");
    rejects([&] { apply_project(Json(bad)); });
    bad["templates"] =
        parse_json(R"({"A":{"extends":"B","fields":{}},"B":{"extends":"A","fields":{}}})");
    rejects([&] { apply_project(Json(bad)); });
}
TEST(data_checkpoints_preserve_exact_state_and_restore_atomically)
{
    Database source(schemas());
    seed(source);
    auto checkpoint = source.checkpoint();
    Database restored(schemas());
    auto stale = restored.begin("example.stale");
    restored.restore_checkpoint(parse_json_exact(to_json(checkpoint)));
    CHECK(to_json(source.snapshot().json()) == to_json(restored.snapshot().json()));
    rejects([&] { stale.commit(); });
    auto retained = restored.snapshot();
    auto next = restored.begin("example.after");
    next.set("Items.Base", "title", val(R"({"type":"string","value":"after"})"));
    CHECK(next.commit() == 2);
    CHECK(retained.revision() == 1);
    auto before = to_json(restored.checkpoint());
    auto wrong = checkpoint.object();
    auto state = wrong.at("state").object();
    state["revision"] = Json(std::string("01"));
    wrong["state"] = Json(state);
    rejects([&] { restored.restore_checkpoint(Json(wrong)); });
    CHECK(to_json(restored.checkpoint()) == before);
    Database mismatched(parse_json(R"({"Other":{"fields":{}}})"));
    rejects([&] { mismatched.restore_checkpoint(checkpoint); });
    Limits low;
    low.bytes = 1;
    Database too_small(schemas(), low);
    rejects([&] { too_small.restore_checkpoint(checkpoint); });
    CHECK(too_small.snapshot().revision() == 0);
}
TEST(data_reload_preserves_last_valid_snapshots_on_parse_and_type_failures)
{
    ProjectSession session;
    std::string project =
        R"({"format":1,"schemas":{"Item":{"fields":{"id":{"type":"u64","required":true}}}},"transactions":[{"owner":"example.reload","operations":[{"op":"create","record":"One","type":"Item"},{"op":"set","record":"One","field":"id","value":{"type":"u64","value":"18446744073709551615"}}]}]})";
    CHECK(session.reload(project));
    CHECK(session.generation() == 1);
    auto old = session.snapshot();
    auto saved = to_json(session.checkpoint());
    CHECK(session.reload(project));
    CHECK(session.generation() == 1);
    CHECK(!session.reload("{"));
    CHECK(!session.error().empty());
    CHECK(session.generation() == 1);
    CHECK(to_json(session.checkpoint()) == saved);
    auto bad = project;
    bad.replace(bad.find("18446744073709551615"), 20, "-1");
    CHECK(!session.reload(bad));
    CHECK(session.generation() == 1);
    auto changed = project;
    changed.replace(changed.find("18446744073709551615"), 20, "2");
    CHECK(session.reload(changed));
    CHECK(session.error().empty());
    CHECK(session.generation() == 2);
    CHECK(old.field("One", "id").json().at("value").string() == "18446744073709551615");
    ProjectSession recovered;
    CHECK(recovered.restore(saved));
    CHECK(to_json(recovered.checkpoint()) == saved);
    CHECK(!recovered.restore("{}"));
    CHECK(to_json(recovered.checkpoint()) == saved);
    bool rejected = false;
    std::thread other(
        [&]
        {
            try
            {
                session.reload(project);
            }
            catch (...)
            {
                rejected = true;
            }
        });
    other.join();
    CHECK(rejected);
    CHECK(session.generation() == 2);
}
TEST(data_reload_schema_changes_are_visible_and_empty_projects_are_stable)
{
    ProjectSession session;
    CHECK(session.reload(R"({"format":1,"schemas":{},"transactions":[]})"));
    CHECK(session.generation() == 0);
    CHECK(session.reload(R"({"format":1,"schemas":{"A":{"fields":{}}},"transactions":[]})"));
    CHECK(session.generation() == 1);
    auto before = to_json(session.checkpoint());
    CHECK(session.reload(R"({"transactions":[],"schemas":{"A":{"fields":{}}},"format":1})"));
    CHECK(session.generation() == 1);
    CHECK(!session.reload(std::string(1048577, ' ')));
    CHECK(!session.restore(std::string(32 * 1048576 + 1, ' ')));
    CHECK(to_json(session.checkpoint()) == before);
}
Json &member(Json &json, std::initializer_list<std::string_view> path)
{
    Json *out = &json;
    for (auto key : path)
        out = &std::get<Json::Object>(out->value).at(std::string(key));
    return *out;
}
TEST(data_checkpoint_rejects_corruption_without_invalidating_live_transactions)
{
    Database db(schemas());
    seed(db);
    auto before = to_json(db.checkpoint());
    auto pending = db.begin("example.pending");
    pending.set("Items.Base", "title", val(R"({"type":"string","value":"pending"})"));
    for (int mode = 0; mode < 8; ++mode)
    {
        auto bad = db.checkpoint();
        if (mode == 0)
            member(bad, {"state", "records", "Items.Base", "fields"}) = Json(Json::Object{});
        if (mode == 1)
            member(bad, {"state", "records", "Items.Base", "provenance", "damage"}) =
                Json(Json::Array{});
        if (mode == 2)
            member(bad, {"state", "commit", "changed_records"}) =
                parse_json(R"(["Items.Base","Items.Base"])");
        if (mode == 3)
            member(bad, {"state", "revision"}) = Json(std::string("0"));
        if (mode == 4)
            member(bad, {"state", "commit", "owner"}) = Json(std::string("invalid"));
        if (mode == 5)
            member(bad, {"state", "records", "Items.Base", "type"}) = Json(std::string("Missing"));
        if (mode >= 6)
        {
            auto &history = std::get<Json::Array>(
                member(bad, {"state", "records", "Items.Base", "provenance", "damage"}).value);
            member(history[0], {mode == 6 ? "sequence" : "operation"}) =
                Json(std::string(mode == 6 ? "0" : "execute"));
        }
        rejects([&] { db.restore_checkpoint(bad); });
        CHECK(to_json(db.checkpoint()) == before);
    }
    CHECK(pending.commit() == 2);
    auto exhausted = db.checkpoint();
    member(exhausted, {"state", "revision"}) = Json(std::string("18446744073709551615"));
    db.restore_checkpoint(exhausted);
    auto tx = db.begin("example.exhausted");
    tx.erase_record("Items.Base");
    rejects([&] { tx.commit(); });
    CHECK(db.snapshot().revision() == UINT64_MAX);
}
TEST(data_composition_bounds_fanout_and_reports_bad_instance_fields)
{
    Json::Object definitions;
    auto base = schemas().at("Item");
    // Two base fields plus a third optional field; 5500 definitions are rejected
    // before expansion, while bounded fanout also has a global expanded-field cap.
    definitions.emplace("Base", base);
    Json::Object wide_fields;
    for (int n = 0; n < 256; ++n)
        wide_fields.emplace("f" + std::to_string(n), parse_json(R"({"type":"u64"})"));
    definitions["Base"] = Json(Json::Object{{"fields", Json(wide_fields)}});
    for (int n = 0; n < 64; ++n)
        definitions.emplace("Child" + std::to_string(n),
                            parse_json(R"({"extends":"Base","fields":{}})"));
    rejects([&] { Database too_wide{Json(definitions)}; });
    auto bad = parse_json(
        R"({"format":1,"schemas":{"A":{"fields":{"x":{"type":"u64"}}}},"templates":{"T":{"type":"A","fields":{"x":{"type":"u64","value":"1"}}}},"transactions":[{"owner":"example.bad","operations":[{"op":"instantiate","record":"Bad","template":"T","fields":{"x":{"type":"i64","value":"1"}}}]}]})");
    rejects([&] { apply_project(bad); });
}
TEST(data_large_checkpoints_round_trip_with_separate_parser_limits)
{
    Json::Array operations{parse_json(R"({"op":"create","record":"Base","type":"Text"})")};
    operations.emplace_back(
        Json::Object{{"op", Json(std::string("set"))},
                     {"record", Json(std::string("Base"))},
                     {"field", Json(std::string("text"))},
                     {"value", Json(Json::Object{{"type", Json(std::string("string"))},
                                                 {"value", Json(std::string(40000, 'x'))}})}});
    for (int i = 0; i < 32; ++i)
        operations.emplace_back(Json::Object{{"op", Json(std::string("clone"))},
                                             {"record", Json("Copy" + std::to_string(i))},
                                             {"from", Json(std::string("Base"))}});
    Json project(Json::Object{
        {"format", Json(1.0)},
        {"schemas", parse_json(R"({"Text":{"fields":{"text":{"type":"string"}}}})")},
        {"transactions",
         Json(Json::Array{Json(Json::Object{{"owner", Json(std::string("example.large"))},
                                            {"operations", Json(operations)}})})}});
    ProjectSession session;
    CHECK(session.reload(to_json(project)));
    auto exported = to_json(session.checkpoint());
    CHECK(exported.size() > 1048576);
    rejects([&] { parse_json_exact(exported); });
    ProjectSession recovered;
    CHECK(recovered.restore(exported));
    CHECK(to_json(recovered.checkpoint()) == exported);
    std::string many = "[0";
    for (size_t n = 0; n < 262144; ++n)
        many += ",0";
    many += "]";
    CHECK(!recovered.restore(many));
    CHECK(recovered.error().find("node") != std::string_view::npos);
    CHECK(to_json(recovered.checkpoint()) == exported);
}
TEST(data_instantiated_defaults_share_immutable_payload_storage)
{
    auto project = parse_json(
        R"({"format":1,"schemas":{"A":{"fields":{"v":{"type":"string"}}}},"templates":{"T":{"type":"A","fields":{"v":{"type":"string","value":"shared immutable default"}}}},"transactions":[{"owner":"example.shared","operations":[{"op":"instantiate","record":"One","template":"T"},{"op":"instantiate","record":"Two","template":"T"}]}]})");
    auto database = build_project(project);
    auto snapshot = database->snapshot();
    CHECK(&snapshot.field("One", "v").encoded() == &snapshot.field("Two", "v").encoded());
    auto tx = database->begin("example.edit");
    tx.set("One", "v", val(R"({"type":"string","value":"changed"})"));
    tx.commit();
    CHECK(database->snapshot().field("Two", "v").json().at("value").string() ==
          "shared immutable default");
}
TEST(data_checkpoint_rejects_locally_impossible_history_and_first_revision)
{
    Database source(schemas());
    seed(source);
    auto checkpoint = source.checkpoint();
    for (int mode = 0; mode < 5; ++mode)
    {
        auto bad = checkpoint;
        if (mode == 0)
        {
            member(bad, {"state", "records"}) = Json(Json::Object{});
        }
        else
        {
            auto &history = std::get<Json::Array>(
                member(bad, {"state", "records", "Items.Base", "provenance", "damage"}).value);
            if (mode == 1)
                member(history[0], {"operation"}) = Json(std::string("remove"));
            if (mode == 2)
                member(history[0], {"owner"}) = Json(std::string("example.forged"));
            if (mode == 3)
            {
                history.push_back(history[0]);
            }
            if (mode == 4)
            {
                auto item = history[0];
                member(item, {"operation"}) = Json(std::string("append"));
                member(item, {"sequence"}) = Json(std::string("4"));
                history.push_back(item);
            }
        }
        rejects([&] { source.restore_checkpoint(bad); });
        CHECK(to_json(source.checkpoint()) == to_json(checkpoint));
    }
}
TEST(data_expanded_registry_accepts_its_exact_byte_boundary)
{
    auto schema = resolve_schemas(parse_json(R"({"A":{"fields":{"v":{"type":"string"}}}})"));
    Json::Object templates;
    for (int i = 0; i < 8; ++i)
        templates.emplace(
            "T" + std::to_string(i),
            parse_json(R"({"type":"A","fields":{"v":{"type":"string","value":""}}})"));
    auto overhead = to_json(resolve_templates(Json(templates), schema)).size();
    auto remaining = 4 * 1048576 - overhead;
    for (int i = 0; i < 8; ++i)
        member(templates.at("T" + std::to_string(i)), {"fields", "v", "value"}) = Json(
            std::string(remaining / 8 + (static_cast<size_t>(i) < remaining % 8 ? 1 : 0), 'x'));
    CHECK(to_json(resolve_templates(Json(templates), schema)).size() == 4 * 1048576);
}
TEST(data_checkpoint_node_cap_is_symmetric_and_checked_before_expansion)
{
    Json::Array elements;
    for (int n = 0; n < 4096; ++n)
        elements.push_back(parse_json(R"({"type":"bool","value":true})"));
    auto value = Value::parse(Json(Json::Object{{"type", Json(std::string("array"))},
                                                {"element", parse_json(R"({"type":"bool"})")},
                                                {"value", Json(elements)}}));
    Database db(
        parse_json(R"({"A":{"fields":{"list":{"type":"array","element":{"type":"bool"}}}}})"));
    auto tx = db.begin("example.nodes");
    tx.create("Base", "A");
    tx.set("Base", "list", value);
    for (int n = 0; n < 22; ++n)
        tx.clone("Copy" + std::to_string(n), "Base");
    tx.commit();
    rejects([&] { db.checkpoint(); });
    CHECK(db.snapshot().record_count() == 23);
    auto remove = db.begin("example.reduce");
    for (int n = 0; n < 22; ++n)
        remove.erase_record("Copy" + std::to_string(n));
    remove.commit();
    auto encoded = to_json(db.checkpoint());
    ProjectSession recovered;
    CHECK(recovered.restore(encoded));
    CHECK(to_json(recovered.checkpoint()) == encoded);
}
int main() { return run_tests(); }
