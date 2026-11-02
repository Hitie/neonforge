#include "script_host.hpp"
#include "test.hpp"
#include <fstream>
#include <thread>

const nf_api *core = nullptr;
const nf_typed_api *typed = nullptr;
nf_str span(std::string_view s) { return {s.data(), s.size()}; }
struct Fixture
{
    nf_runtime *runtime = nullptr;
    std::filesystem::path root;
    std::unique_ptr<nf::ScriptHost> scripts;
    Fixture(nf::ScriptLimits limits = {})
    {
        static int serial = 0;
        root = std::filesystem::current_path() / ("script-test-" + std::to_string(++serial));
        std::filesystem::create_directories(root / "mods");
        auto data = (root / "data").generic_string();
        nf_runtime_options opts{sizeof(opts), 0, span(data)};
        CHECK(core->runtime_create(&opts, &runtime) == NF_OK);
        scripts = std::make_unique<nf::ScriptHost>(core, typed, runtime, limits);
        file("init.lua", "");
    }
    void file(std::string_view name, std::string_view source)
    {
        auto path = root / "mods" / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << source;
    }
    bool load(std::string id = "script.one", std::string entry = "init.lua")
    {
        return scripts->load(std::move(id), root / "mods", std::move(entry));
    }
    ~Fixture()
    {
        scripts.reset();
        core->runtime_destroy(runtime);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};
TEST(script_globals_and_libraries_are_isolated)
{
    Fixture f;
    CHECK(f.load("script.one") && f.load("script.two"));
    CHECK(f.scripts->execute(
        "script.one",
        "private_value=42; assert(io==nil and os==nil and debug==nil and package==nil and "
        "coroutine==nil and load==nil and dofile==nil); assert(type(pcall)=='function')"));
    CHECK(f.scripts->execute("script.two", "assert(private_value==nil); private_value=9"));
    CHECK(f.scripts->execute("script.one", "assert(private_value==42)"));
}
TEST(script_errors_are_attributed_and_do_not_fault_other_mods)
{
    Fixture f;
    CHECK(f.load("script.one") && f.load("script.two"));
    CHECK(!f.scripts->execute("script.one", "error('fixture failure')"));
    CHECK(f.scripts->error().find("script.one") != std::string::npos);
    CHECK(f.scripts->stats("script.one").faulted);
    CHECK(f.scripts->execute("script.two", "assert(20+22==42)"));
}
TEST(script_instruction_limit_cannot_be_caught_with_pcall)
{
    Fixture f({1048576, 10000, 32});
    CHECK(f.load());
    CHECK(!f.scripts->execute("script.one",
                              "while true do pcall(function() for i=1,100 do end end) end"));
    CHECK(f.scripts->stats("script.one").faulted);
    CHECK(f.scripts->error().find("instruction") != std::string::npos);
}
TEST(script_memory_failure_is_contained)
{
    Fixture f({262144, 100000, 32});
    CHECK(f.load());
    CHECK(!f.scripts->execute("script.one",
                              "local t={} while true do t[#t+1]=string.rep('x',10000) end"));
    CHECK(f.scripts->stats("script.one").peak_bytes <= 262144);
    CHECK(f.scripts->stats("script.one").faulted);
}
TEST(script_modules_are_local_cached_and_reject_path_escape)
{
    Fixture f;
    f.file("helper.lua", "module_loads=(module_loads or 0)+1; return {answer=42}");
    CHECK(f.load());
    if (!f.scripts->execute("script.one",
                            "assert(require('helper').answer==42); "
                            "assert(require('helper').answer==42 and module_loads==1); assert(not "
                            "pcall(require,'../escape')); assert(not pcall(require,'missing'))"))
        throw std::runtime_error(f.scripts->error());
}
TEST(script_typed_calls_round_trip_full_width_integers)
{
    Fixture f;
    f.file("provider.lua", "nf.export('script.echo','u64',{'u64'},function(x) return x end); "
                           "nf.export('script.signed','i64',{'i64'},function(x) return x end)");
    CHECK(f.load("script.provider", "provider.lua") && f.load("script.consumer"));
    CHECK(f.scripts->execute("script.consumer",
                             "local n=nf.u64('18446744073709551615'); "
                             "assert(tostring(nf.call('script.echo',n))=='18446744073709551615'); "
                             "assert(nf.call('script.signed',math.mininteger)==math.mininteger); "
                             "local s=nf.signature('script.echo'); assert(s.result=='u64' and "
                             "s.arguments[1]=='u64'); assert(not pcall(nf.call,'script.echo',42)); "
                             "assert(not pcall(nf.u64,'18446744073709551616'))"));
    nf_typed_stats stats{sizeof(stats)};
    CHECK(typed->stats(f.runtime, &stats) == NF_OK && stats.results == 0);
}
TEST(script_module_instruction_limit_escapes_protected_require)
{
    Fixture f({1048576, 10000, 32});
    f.file("expensive.lua", "local total=0; for i=1,10000000 do total=total+i end; return total");
    CHECK(f.load());
    CHECK(!f.scripts->execute("script.one", "pcall(require,'expensive')"));
    CHECK(f.scripts->stats("script.one").faulted);
}
TEST(script_callbacks_self_remove_cancel_and_share_exact_settings)
{
    Fixture f;
    CHECK(f.load());
    CHECK(f.scripts->execute(
        "script.one",
        "hits=0; local token; token=nf.on('script.event',function(payload) "
        "assert(payload=='a\\0b'); hits=hits+1; nf.off(token) end); "
        "nf.emit('script.event','a\\0b'); nf.emit('script.event','a\\0b'); assert(hits==1); local "
        "t=nf.defer(function() error('cancelled task ran') end); nf.cancel(t); nf.defer(function() "
        "nf.settings_write('answer','{\"answer\":42}') end)"));
    uint32_t ran = 0;
    CHECK(core->task_pump(f.runtime, 20, &ran) == NF_OK && ran == 1);
    CHECK(f.scripts->execute("script.one", "assert(nf.settings_read('answer')=='{\"answer\":42}'); "
                                           "assert(nf.settings_read('missing')==nil)"));
    CHECK(f.scripts->stats("script.one").bindings == 0);
}
TEST(script_native_result_is_released_when_lua_allocation_fails)
{
    Fixture f({262144, 100000, 32});
    nf_context provider = 0;
    CHECK(core->context_create(f.runtime, span("script.native"), &provider) == NF_OK);
    struct Payload
    {
        std::string bytes = std::string(1048576, 'x');
        int calls = 0;
    } payload;
    auto callback = +[](nf_context, nf_context, const nf_value *, uint32_t, nf_value *out,
                        void *user) -> nf_status
    {
        auto &data = *static_cast<Payload *>(user);
        ++data.calls;
        out->kind = NF_VALUE_BYTES;
        out->as.bytes = {data.bytes.data(), data.bytes.size()};
        return NF_OK;
    };
    nf_signature signature{sizeof(signature), NF_VALUE_BYTES, 0, 0, nullptr};
    nf_token fn = 0;
    CHECK(typed->function_register(f.runtime, provider, span("script.large"), &signature, callback,
                                   &payload, &fn) == NF_OK);
    CHECK(f.load());
    CHECK(
        f.scripts->execute("script.one", "local ok=pcall(nf.call,'script.large'); assert(not ok)"));
    CHECK(!f.scripts->stats("script.one").faulted);
    CHECK(payload.calls == 1);
    nf_typed_stats stats{sizeof(stats)};
    CHECK(typed->stats(f.runtime, &stats) == NF_OK && stats.results == 0 &&
          stats.retained_payload_bytes == 0);
    CHECK(core->context_close(f.runtime, provider) == NF_OK &&
          core->context_release(f.runtime, provider) == NF_OK);
}
TEST(script_reload_reclaims_old_callbacks_and_preserves_settings)
{
    Fixture f;
    f.file("init.lua",
           "nf.on('script.tick',function() nf.settings_write('answer','{\"answer\":42}') end); "
           "nf.defer(function() end); nf.export('script.once','void',{},function() end)");
    CHECK(f.load());
    CHECK(f.scripts->execute("script.one", "nf.settings_write('before_reload','{\"kept\":true}')"));
    for (int i = 0; i < 100; ++i)
    {
        CHECK(f.scripts->reload("script.one"));
        CHECK(f.scripts->stats("script.one").bindings == 3);
        CHECK(f.scripts->stats("script.one").memory_bytes <= 1048576);
        nf_typed_stats typed_stats{sizeof(typed_stats)};
        CHECK(typed->stats(f.runtime, &typed_stats) == NF_OK && typed_stats.functions == 1 &&
              typed_stats.results == 0 && typed_stats.retained_payload_bytes == 0);
        CHECK(f.scripts->execute("script.one",
                                 "assert(nf.settings_read('before_reload')=='{\"kept\":true}')"));
        nf_stats stats{sizeof(stats)};
        CHECK(core->stats(f.runtime, &stats) == NF_OK && stats.contexts == 1 &&
              stats.subscriptions == 1 && stats.queued_tasks == 1);
    }
    CHECK(f.scripts->execute(
        "script.one",
        "nf.emit('script.tick',''); assert(nf.settings_read('answer')=='{\"answer\":42}')"));
    CHECK(f.scripts->unload("script.one"));
    nf_stats stats{sizeof(stats)};
    CHECK(core->stats(f.runtime, &stats) == NF_OK && stats.contexts == 0 &&
          stats.subscriptions == 0 && stats.queued_tasks == 0);
}
TEST(script_host_rejects_incompatible_api_tables)
{
    Fixture f;
    auto incompatible = *typed;
    incompatible.abi_version = 99;
    rejects([&] { nf::ScriptHost host(core, &incompatible, f.runtime); });
    auto bad_core = *core;
    bad_core.abi_version = 99;
    rejects([&] { nf::ScriptHost host(&bad_core, typed, f.runtime); });
}
TEST(script_binding_limits_and_callback_fault_cleanup)
{
    Fixture f({1048576, 1000000, 2});
    CHECK(f.load());
    CHECK(f.scripts->execute(
        "script.one",
        "for i=1,1000 do local h=nf.on('script.churn',function() end); assert(nf.off(h)); "
        "assert(not nf.off(h)) end; local a=nf.defer(function() end); local b=nf.defer(function() "
        "end); assert(not pcall(nf.defer,function() end)); nf.cancel(a); nf.cancel(b); "
        "nf.on('script.fail',function() error('callback fixture') end)"));
    CHECK(!f.scripts->execute("script.one", "nf.emit('script.fail','')"));
    nf_stats stats{sizeof(stats)};
    CHECK(core->stats(f.runtime, &stats) == NF_OK && stats.subscriptions == 0 &&
          stats.queued_tasks == 0);
    CHECK(f.scripts->stats("script.one").faulted);
}
TEST(script_callbacks_keep_nested_state_and_typed_validation)
{
    Fixture f;
    CHECK(f.load());
    CHECK(f.scripts->execute(
        "script.one",
        "nf.export('script.text','utf8',{'utf8'},function(x) return x end); "
        "nf.export('script.float','f64',{'f64'},function(x) return x end); "
        "assert(nf.call('script.text','a\\0b')=='a\\0b'); "
        "assert(nf.call('script.float',42)==42.0); assert(not "
        "pcall(nf.call,'script.float',math.maxinteger)); assert(not "
        "pcall(nf.call,'script.text','\\255')); count=0; nf.on('script.nested',function() "
        "count=count+1; if count<4 then nf.emit('script.nested','') end end); "
        "nf.emit('script.nested',''); assert(count==4)"));
}
TEST(script_kind_names_reject_embedded_nul)
{
    Fixture f;
    CHECK(f.load());
    CHECK(f.scripts->execute(
        "script.one",
        "assert(not pcall(nf.export,'script.badkind','void\\0extra',{},function() end))"));
}
TEST(script_initialization_oom_and_bytecode_are_contained)
{
    Fixture f({65536, 100000, 32});
    f.file("large.lua", "value='" + std::string(100000, 'x') + "'");
    CHECK(!f.load("script.large", "large.lua"));
    CHECK(f.scripts->stats("script.large").faulted &&
          f.scripts->stats("script.large").peak_bytes <= 65536);
    CHECK(f.scripts->unload("script.large"));
    f.file("binary.lua", std::string("\x1bLua", 4));
    CHECK(!f.load("script.binary", "binary.lua"));
}
TEST(script_host_rejects_calls_from_other_threads)
{
    Fixture f;
    CHECK(f.load());
    bool rejected = false;
    std::thread other(
        [&]
        {
            try
            {
                f.scripts->execute("script.one", "error('must not run')");
            }
            catch (const std::runtime_error &)
            {
                rejected = true;
            }
        });
    other.join();
    CHECK(rejected && f.scripts->execute("script.one", "assert(true)"));
}
TEST(script_error_reports_the_most_recent_failed_operation)
{
    Fixture f;
    CHECK(f.load("script.a") && f.load("script.b"));
    CHECK(!f.scripts->execute("script.a", "error('first')"));
    CHECK(!f.scripts->execute("script.b", "error('second')"));
    CHECK(f.scripts->error().find("script.b") != std::string::npos);
    CHECK(!f.load("script.missing", "missing.lua"));
    CHECK(f.scripts->error().find("script.missing") != std::string::npos);
}
TEST(script_protected_calls_preserve_results_and_error_handlers)
{
    Fixture f;
    CHECK(f.load());
    CHECK(f.scripts->execute(
        "script.one",
        "local ok,a,b,c=pcall(function(x) return x,nil,7 end,42); assert(ok and a==42 and b==nil "
        "and c==7); local good,x,y=xpcall(function(a,b) return a,b end,error,4,5); assert(good and "
        "x==4 and y==5); local good,message=xpcall(function() error('marker') end,function(e) "
        "return 'handled:'..e end); assert(not good and string.find(message,'handled:') and "
        "string.find(message,'marker'))"));
}
TEST(script_relative_root_is_stable_after_working_directory_changes)
{
    Fixture f;
    f.file("helper.lua", "return 42");
    auto old = std::filesystem::current_path();
    struct Restore
    {
        std::filesystem::path p;
        ~Restore() { std::filesystem::current_path(p); }
    } restore{old};
    CHECK(f.scripts->load("script.relative", std::filesystem::relative(f.root / "mods", old),
                          "init.lua"));
    std::filesystem::create_directories(f.root / "other");
    std::filesystem::current_path(f.root / "other");
    CHECK(f.scripts->execute("script.relative", "assert(require('helper')==42)"));
    CHECK(f.scripts->reload("script.relative"));
}
int main()
{
    if (nf_get_api(1, sizeof(nf_api), &core) != NF_OK ||
        nf_get_typed_api(1, sizeof(nf_typed_api), &typed) != NF_OK)
        return 1;
    return run_tests();
}
