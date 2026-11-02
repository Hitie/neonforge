#include "test.hpp"
#include <filesystem>
#include <fstream>
#include <neonforge/api.h>
#include <thread>
#include <windows.h>
nf_str s(std::string_view v) { return {v.data(), v.size()}; }
const nf_api *api = nullptr;
struct Fixture
{
    nf_runtime *r = nullptr;
    nf_context a = 0, b = 0;
    std::filesystem::path root;
    Fixture()
    {
        static unsigned n = 0;
        root = std::filesystem::current_path() /
               ("test-data-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++n));
        auto u = root.u8string();
        std::string path(reinterpret_cast<const char *>(u.data()), u.size());
        nf_runtime_options o{sizeof(o), 0, s(path)};
        CHECK(api->runtime_create(&o, &r) == NF_OK);
        CHECK(api->context_create(r, s("com.test.a"), &a) == NF_OK);
        CHECK(api->context_create(r, s("com.test.b"), &b) == NF_OK);
    }
    ~Fixture()
    {
        if (r)
        {
            api->context_close(r, a);
            api->context_close(r, b);
            api->context_release(r, a);
            api->context_release(r, b);
            api->runtime_destroy(r);
        }
    }
    nf_stats stats()
    {
        nf_stats x{};
        x.struct_size = sizeof(x);
        CHECK(api->stats(r, &x) == NF_OK);
        return x;
    }
};
void NF_CALL count(nf_context, const nf_event *, void *u) { ++*static_cast<int *>(u); }
void NF_CALL task_count(nf_context, void *u) { ++*static_cast<int *>(u); }
TEST(abi_rejects_before_exposing_table)
{
    const nf_api *p = reinterpret_cast<const nf_api *>(1);
    CHECK(nf_get_api(999, sizeof(nf_api), &p) == NF_E_ABI);
    CHECK(!p);
    CHECK(nf_get_api(1, 4, &p) == NF_E_ABI);
    CHECK(!p);
    CHECK(nf_get_api(1, sizeof(nf_api), nullptr) == NF_E_ARGUMENT);
}
TEST(context_generation_and_owner_isolation)
{
    Fixture f;
    nf_token t = 0;
    int n = 0;
    CHECK(api->event_subscribe(f.r, f.a, s("test.event"), count, &n, &t) == NF_OK);
    CHECK(api->event_unsubscribe(f.r, f.b, t) == NF_E_OWNER);
    CHECK(api->event_emit(f.r, f.b, s("test.event"), {}) == NF_OK);
    CHECK(n == 1);
    auto old = f.a;
    CHECK(api->context_close(f.r, f.a) == NF_OK);
    CHECK(api->context_release(f.r, f.a) == NF_OK);
    f.a = 0;
    nf_context newer = 0;
    CHECK(api->context_create(f.r, s("com.test.a"), &newer) == NF_OK);
    CHECK(newer != old);
    CHECK(api->event_emit(f.r, old, s("test.event"), {}) == NF_E_HANDLE);
    f.a = newer;
}
TEST(callback_mutation_and_exception_isolation)
{
    Fixture f;
    struct State
    {
        Fixture *f;
        nf_token other = 0;
        int first = 0, second = 0, added = 0;
    } st{&f};
    auto first = +[](nf_context c, const nf_event *, void *u)
    {
        auto &x = *static_cast<State *>(u);
        ++x.first;
        api->event_unsubscribe(x.f->r, c, x.other);
        nf_token t = 0;
        api->event_subscribe(x.f->r, c, s("test.event"), count, &x.added, &t);
    };
    nf_token t = 0;
    CHECK(api->event_subscribe(f.r, f.a, s("test.event"), first, &st, &t) == NF_OK);
    CHECK(api->event_subscribe(f.r, f.a, s("test.event"), count, &st.second, &st.other) == NF_OK);
    CHECK(api->event_emit(f.r, f.b, s("test.event"), {}) == NF_OK);
    CHECK(st.first == 1 && st.second == 0 && st.added == 0);
    CHECK(api->event_emit(f.r, f.b, s("test.event"), {}) == NF_OK);
    CHECK(st.added == 1);
    auto bad =
        +[](nf_context, const nf_event *, void *) { throw std::runtime_error("fixture error"); };
    CHECK(api->event_subscribe(f.r, f.a, s("test.bad"), bad, nullptr, &t) == NF_OK);
    CHECK(api->event_emit(f.r, f.b, s("test.bad"), {}) == NF_E_CALLBACK);
    CHECK(f.stats().callback_errors == 1);
}
TEST(task_snapshot_cancellation_and_cleanup)
{
    Fixture f;
    struct State
    {
        Fixture *f;
        int n = 0;
    } st{&f};
    auto later = +[](nf_context c, void *u)
    {
        auto &x = *static_cast<State *>(u);
        ++x.n;
        nf_token t = 0;
        api->task_enqueue(x.f->r, c, task_count, &x.n, &t);
    };
    nf_token t = 0;
    CHECK(api->task_enqueue(f.r, f.a, later, &st, &t) == NF_OK);
    uint32_t ran = 0;
    CHECK(api->task_pump(f.r, 100, &ran) == NF_OK);
    CHECK(ran == 1 && st.n == 1);
    CHECK(f.stats().queued_tasks == 1);
    CHECK(api->context_close(f.r, f.a) == NF_OK);
    CHECK(api->task_pump(f.r, 100, &ran) == NF_OK);
    CHECK(st.n == 1 && ran == 0);
    CHECK(f.stats().queued_tasks == 0);
}
TEST(task_owner_and_wrong_thread)
{
    Fixture f;
    int n = 0;
    nf_token t = 0;
    CHECK(api->task_enqueue(f.r, f.a, task_count, &n, &t) == NF_OK);
    CHECK(api->task_cancel(f.r, f.b, t) == NF_E_OWNER);
    nf_status other = NF_OK;
    std::thread worker(
        [&]
        {
            uint32_t ran = 0;
            other = api->task_pump(f.r, 1, &ran);
        });
    worker.join();
    CHECK(other == NF_E_THREAD);
    CHECK(api->task_cancel(f.r, f.a, t) == NF_OK);
    uint32_t ran = 0;
    CHECK(api->task_pump(f.r, 1, &ran) == NF_OK);
    CHECK(n == 0);
}
TEST(service_lease_pins_provider_until_consumer_cleanup)
{
    Fixture f;
    int table = 42;
    nf_token token = 0;
    CHECK(api->service_publish(f.r, f.a, s("com.test.service"), 1, &table, sizeof(table), &token) ==
          NF_OK);
    nf_service lease{};
    CHECK(api->service_acquire(f.r, f.b, s("com.test.service"), 1, sizeof(int), &lease) == NF_OK);
    CHECK(*static_cast<const int *>(lease.table) == 42);
    CHECK(api->service_release(f.r, f.a, lease.lease) == NF_E_OWNER);
    CHECK(api->context_close(f.r, f.a) == NF_OK);
    CHECK(api->context_release(f.r, f.a) == NF_E_BUSY);
    nf_service second{};
    CHECK(api->service_acquire(f.r, f.b, s("com.test.service"), 1, sizeof(int), &second) ==
          NF_E_UNAVAILABLE);
    CHECK(api->context_close(f.r, f.b) == NF_OK);
    CHECK(api->context_release(f.r, f.a) == NF_OK);
    f.a = 0;
    CHECK(f.stats().leases == 0);
}
TEST(settings_persist_and_reject_invalid_json_and_keys)
{
    Fixture f;
    CHECK(api->settings_write(f.r, f.a, s("settings"), s("{\"version\":1,\"enabled\":true}")) ==
          NF_OK);
    uint64_t needed = 0;
    CHECK(api->settings_read(f.r, f.a, s("settings"), nullptr, 0, &needed) == NF_E_BUFFER);
    std::string value(needed, '\0');
    CHECK(api->settings_read(f.r, f.a, s("settings"), value.data(), value.size(), &needed) ==
          NF_OK);
    CHECK(value.find("enabled") != value.npos);
    CHECK(api->settings_write(f.r, f.a, s("settings"), s("{")) == NF_E_ARGUMENT);
    CHECK(api->settings_write(f.r, f.a, s("../escape"), s("{}")) == NF_E_ARGUMENT);
    CHECK(!std::filesystem::exists(f.root / "escape.json"));
    nf_context old = f.a;
    CHECK(api->context_close(f.r, old) == NF_OK);
    CHECK(api->context_release(f.r, old) == NF_OK);
    CHECK(api->context_create(f.r, s("com.test.a"), &f.a) == NF_OK);
    CHECK(api->settings_read(f.r, f.a, s("settings"), value.data(), value.size(), &needed) ==
          NF_OK);
    CHECK(value.find("true") != value.npos);
}
TEST(capabilities_do_not_fabricate_game_support)
{
    Fixture f;
    nf_capability c{};
    c.struct_size = sizeof(c);
    CHECK(api->capability_query(f.r, s("nf.events"), &c) == NF_OK);
    CHECK(c.implemented && c.version == 1 && !c.game_tested);
    CHECK(api->capability_query(f.r, s("nf.game.world"), &c) == NF_E_UNAVAILABLE);
    CHECK(!c.implemented && !c.offline_tested && !c.game_tested);
}
TEST(runtime_cannot_destroy_live_contexts_and_100_cycles)
{
    Fixture f;
    CHECK(api->runtime_destroy(f.r) == NF_E_BUSY);
    for (int i = 0; i < 100; ++i)
    {
        nf_context c = 0;
        CHECK(api->context_create(f.r, s("com.test.cycle"), &c) == NF_OK);
        nf_token t = 0;
        int n = 0;
        CHECK(api->event_subscribe(f.r, c, s("test.event"), count, &n, &t) == NF_OK);
        CHECK(api->task_enqueue(f.r, c, task_count, &n, &t) == NF_OK);
        CHECK(api->context_close(f.r, c) == NF_OK);
        CHECK(api->context_release(f.r, c) == NF_OK);
    }
    auto st = f.stats();
    CHECK(st.contexts == 2 && st.subscriptions == 0 && st.queued_tasks == 0 && st.services == 0 &&
          st.leases == 0);
}
TEST(context_metrics_attribute_work_and_resource_counts)
{
    Fixture f;
    CHECK(api->context_stats);
    nf_token t = 0;
    int n = 0;
    CHECK(api->event_subscribe(f.r, f.a, s("test.event"), count, &n, &t) == NF_OK);
    CHECK(api->event_emit(f.r, f.b, s("test.event"), {}) == NF_OK);
    nf_context_stats a{}, b{};
    a.struct_size = sizeof(a);
    b.struct_size = sizeof(b);
    CHECK(api->context_stats(f.r, f.a, &a) == NF_OK);
    CHECK(api->context_stats(f.r, f.b, &b) == NF_OK);
    CHECK(a.callbacks == 1 && a.subscriptions == 1 && b.callbacks == 0 && b.subscriptions == 0);
}
TEST(logs_cannot_append_through_hardlinks)
{
    Fixture f;
    auto dir = f.root / "logs";
    std::filesystem::create_directory(dir);
    auto outside = f.root / "outside.txt";
    {
        std::ofstream out(outside);
        out << "keep";
    }
    std::filesystem::create_hard_link(outside, dir / "neonforge.jsonl");
    CHECK(api->log(f.r, f.a, 1, s("must not append")) == NF_E_ARGUMENT);
    std::ifstream in(outside);
    std::string value{std::istreambuf_iterator<char>(in), {}};
    CHECK(value == "keep");
}
TEST(executing_owner_cannot_be_released_during_callback)
{
    Fixture f;
    struct State
    {
        Fixture *f;
        nf_status released = NF_OK;
    } st{&f};
    auto callback = +[](nf_context c, const nf_event *, void *u)
    {
        auto &x = *static_cast<State *>(u);
        api->context_close(x.f->r, c);
        x.released = api->context_release(x.f->r, c);
    };
    nf_token t = 0;
    CHECK(api->event_subscribe(f.r, f.a, s("test.event"), callback, &st, &t) == NF_OK);
    CHECK(api->event_emit(f.r, f.b, s("test.event"), {}) == NF_OK);
    CHECK(st.released == NF_E_BUSY);
    CHECK(api->context_release(f.r, f.a) == NF_OK);
    f.a = 0;
}
TEST(nested_dispatch_is_bounded)
{
    Fixture f;
    struct State
    {
        Fixture *f;
        int calls = 0;
        nf_status last = NF_OK;
    } st{&f};
    auto callback = +[](nf_context c, const nf_event *, void *u)
    {
        auto &x = *static_cast<State *>(u);
        ++x.calls;
        auto result = api->event_emit(x.f->r, c, s("test.recursion"), {});
        if (result != NF_OK)
            x.last = result;
    };
    nf_token t = 0;
    CHECK(api->event_subscribe(f.r, f.a, s("test.recursion"), callback, &st, &t) == NF_OK);
    CHECK(api->event_emit(f.r, f.b, s("test.recursion"), {}) == NF_OK);
    CHECK(st.calls == 16 && st.last == NF_E_LIMIT);
}
TEST(settings_preserve_large_integer_and_bounded_escaped_text)
{
    Fixture f;
    const std::string exact = "{\"id\":9007199254740993}";
    CHECK(api->settings_write(f.r, f.a, s("exact"), s(exact)) == NF_OK);
    uint64_t needed = 0;
    CHECK(api->settings_read(f.r, f.a, s("exact"), nullptr, 0, &needed) == NF_E_BUFFER);
    std::string output(static_cast<size_t>(needed), '\0');
    CHECK(api->settings_read(f.r, f.a, s("exact"), output.data(), output.size(), &needed) == NF_OK);
    output.pop_back();
    CHECK(output == exact);
    std::string escaped = "\"";
    for (int i = 0; i < 200000; ++i)
        escaped += "\\n";
    escaped += '"';
    CHECK(api->settings_write(f.r, f.a, s("escaped"), s(escaped)) == NF_OK);
    CHECK(api->settings_read(f.r, f.a, s("escaped"), nullptr, 0, &needed) == NF_E_BUFFER);
    CHECK(needed == escaped.size() + 1);
    output.assign(static_cast<size_t>(needed), '\0');
    CHECK(api->settings_read(f.r, f.a, s("escaped"), output.data(), output.size(), &needed) ==
          NF_OK);
}
TEST(expired_runtime_handles_never_revalidate)
{
    Fixture f;
    auto u = f.root.u8string();
    std::string root(reinterpret_cast<const char *>(u.data()), u.size());
    nf_runtime_options options{sizeof(options), 0, s(root)};
    std::vector<nf_runtime *> expired;
    for (int i = 0; i < 64; ++i)
    {
        nf_runtime *current = nullptr;
        CHECK(api->runtime_create(&options, &current) == NF_OK);
        for (auto old : expired)
        {
            nf_stats stats{};
            stats.struct_size = sizeof(stats);
            CHECK(api->stats(old, &stats) == NF_E_HANDLE);
            CHECK(api->runtime_destroy(old) == NF_E_HANDLE);
        }
        CHECK(api->runtime_destroy(current) == NF_OK);
        expired.push_back(current);
    }
}
int main()
{
    if (nf_get_api(1, sizeof(nf_api), &api) != NF_OK)
    {
        std::cout << "FAIL ABI negotiation: runtime not implemented\n";
        return 1;
    }
    return run_tests();
}
