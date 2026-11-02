#include "test.hpp"
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <neonforge/typed.h>
#include <thread>
#include <windows.h>

const nf_api *core = nullptr;
const nf_typed_api *typed = nullptr;
using GetTyped = nf_status(NF_CALL *)(uint32_t, uint32_t, const nf_typed_api **);
GetTyped get_typed = nullptr;
nf_str span(std::string_view s) { return {s.data(), s.size()}; }
nf_value value(uint32_t kind)
{
    nf_value v{};
    v.struct_size = sizeof(v);
    v.kind = kind;
    return v;
}
struct Fixture
{
    nf_runtime *runtime = nullptr;
    nf_context a = 0, b = 0;
    Fixture()
    {
        auto path = (std::filesystem::current_path() / "typed-test-data").u8string();
        std::string root(reinterpret_cast<const char *>(path.data()), path.size());
        nf_runtime_options opts{sizeof(opts), 0, span(root)};
        CHECK(core->runtime_create(&opts, &runtime) == NF_OK);
        CHECK(core->context_create(runtime, span("typed.provider"), &a) == NF_OK);
        CHECK(core->context_create(runtime, span("typed.consumer"), &b) == NF_OK);
    }
    ~Fixture()
    {
        for (auto c : {a, b})
            if (c)
            {
                core->context_close(runtime, c);
                core->context_release(runtime, c);
            }
        core->runtime_destroy(runtime);
    }
    nf_typed_stats stats()
    {
        nf_typed_stats s{};
        s.struct_size = sizeof(s);
        CHECK(typed->stats(runtime, &s) == NF_OK);
        return s;
    }
};
nf_status NF_CALL echo(nf_context, nf_context, const nf_value *args, uint32_t, nf_value *out,
                       void *)
{
    *out = args[0];
    return NF_OK;
}
nf_token register_echo(Fixture &f, uint32_t kind = NF_VALUE_U64)
{
    nf_signature signature{sizeof(signature), kind, 1, 0, &kind};
    nf_token fn = 0;
    CHECK(typed->function_register(f.runtime, f.a, span("typed.echo"), &signature, echo, nullptr,
                                   &fn) == NF_OK);
    return fn;
}
TEST(extension_negotiation_rejects_truncated_tables)
{
    const nf_typed_api *out = reinterpret_cast<const nf_typed_api *>(1);
    CHECK(get_typed(999, sizeof(nf_typed_api), &out) == NF_E_ABI && !out);
    CHECK(get_typed(1, 4, &out) == NF_E_ABI && !out);
    CHECK(get_typed(1, sizeof(nf_typed_api), nullptr) == NF_E_ARGUMENT);
}
TEST(numeric_conversion_preserves_integer_boundaries)
{
    auto in = value(NF_VALUE_U64), out = value(NF_VALUE_VOID);
    in.as.u64 = UINT64_MAX;
    CHECK(typed->value_convert(&in, NF_VALUE_U64, &out) == NF_OK && out.as.u64 == UINT64_MAX);
    CHECK(typed->value_convert(&in, NF_VALUE_I64, &out) == NF_E_ARGUMENT);
    CHECK(typed->value_convert(&in, NF_VALUE_F64, &out) == NF_E_ARGUMENT);
    in.as.u64 = uint64_t(1) << 63;
    CHECK(typed->value_convert(&in, NF_VALUE_F64, &out) == NF_OK && out.as.f64 == 0x1p63);
    in = value(NF_VALUE_I64);
    in.as.i64 = INT64_MIN;
    CHECK(typed->value_convert(&in, NF_VALUE_F64, &out) == NF_OK && out.as.f64 == -0x1p63);
    CHECK(typed->value_convert(&in, NF_VALUE_U64, &out) == NF_E_ARGUMENT);
    in = value(NF_VALUE_F64);
    in.as.f64 = 0x1p64;
    CHECK(typed->value_convert(&in, NF_VALUE_U64, &out) == NF_E_ARGUMENT);
    in.as.f64 = 0x1p63;
    CHECK(typed->value_convert(&in, NF_VALUE_I64, &out) == NF_E_ARGUMENT);
    in.as.f64 = -0x1p63;
    CHECK(typed->value_convert(&in, NF_VALUE_I64, &out) == NF_OK && out.as.i64 == INT64_MIN);
    in.as.f64 = 1.5;
    CHECK(typed->value_convert(&in, NF_VALUE_I64, &out) == NF_E_ARGUMENT);
    in.as.f64 = std::numeric_limits<double>::infinity();
    CHECK(typed->value_convert(&in, NF_VALUE_F64, &out) == NF_E_ARGUMENT);
    in.as.f64 = std::numeric_limits<double>::quiet_NaN();
    CHECK(typed->value_convert(&in, NF_VALUE_F64, &out) == NF_E_ARGUMENT);
}
TEST(signature_lookup_and_exact_u64_invocation)
{
    Fixture f;
    auto fn = register_echo(f);
    nf_function_info info{};
    info.struct_size = sizeof(info);
    CHECK(typed->function_resolve(f.runtime, f.b, span("typed.echo"), &info, nullptr, 0) ==
          NF_E_BUFFER);
    CHECK(info.function == fn && info.argument_count == 1 && info.result_kind == NF_VALUE_U64 &&
          info.owner == f.a);
    uint32_t kind = 0;
    CHECK(typed->function_resolve(f.runtime, f.b, span("typed.echo"), &info, &kind, 1) == NF_OK &&
          kind == NF_VALUE_U64);
    auto in = value(NF_VALUE_U64);
    in.as.u64 = UINT64_MAX;
    nf_token result = 0;
    CHECK(typed->function_invoke(f.runtime, f.b, fn, &in, 1, &result) == NF_OK && result);
    auto out = value(NF_VALUE_VOID);
    uint64_t needed = 99;
    CHECK(typed->result_read(f.runtime, f.b, result, &out, nullptr, 0, &needed) == NF_OK &&
          needed == 0);
    CHECK(out.kind == NF_VALUE_U64 && out.as.u64 == UINT64_MAX);
    CHECK(typed->result_read(f.runtime, f.a, result, &out, nullptr, 0, &needed) == NF_E_OWNER);
    CHECK(typed->result_release(f.runtime, f.a, result) == NF_E_OWNER);
    CHECK(typed->result_release(f.runtime, f.b, result) == NF_OK);
    CHECK(typed->result_release(f.runtime, f.b, result) == NF_E_HANDLE);
}
TEST(invocation_rejects_wrong_arguments_before_callback)
{
    Fixture f;
    auto fn = register_echo(f);
    auto in = value(NF_VALUE_I64);
    in.as.i64 = 4;
    nf_token result = 99;
    CHECK(typed->function_invoke(f.runtime, f.b, fn, &in, 1, &result) == NF_E_ARGUMENT && !result);
    CHECK(typed->function_invoke(f.runtime, f.b, fn, nullptr, 0, &result) == NF_E_ARGUMENT);
    CHECK(typed->function_invoke(f.runtime, f.b, fn, nullptr, 1, &result) == NF_E_ARGUMENT);
    CHECK(f.stats().results == 0);
}
TEST(text_and_bytes_are_owned_and_short_buffers_are_preserved)
{
    for (auto kind : {NF_VALUE_UTF8, NF_VALUE_BYTES})
    {
        Fixture f;
        auto fn = register_echo(f, kind);
        std::string data = "hello";
        data.push_back('\0');
        data += "world";
        auto in = value(kind);
        if (kind == NF_VALUE_UTF8)
            in.as.text = span(data);
        else
            in.as.bytes = {data.data(), data.size()};
        nf_token result = 0;
        CHECK(typed->function_invoke(f.runtime, f.b, fn, &in, 1, &result) == NF_OK);
        data.assign("changed");
        CHECK(typed->function_unregister(f.runtime, f.b, fn) == NF_E_OWNER);
        CHECK(typed->function_unregister(f.runtime, f.a, fn) == NF_OK);
        auto out = value(NF_VALUE_VOID);
        uint64_t needed = 0;
        char buffer[16];
        std::memset(buffer, 'x', sizeof(buffer));
        CHECK(typed->result_read(f.runtime, f.b, result, &out, buffer, 2, &needed) == NF_E_BUFFER);
        CHECK(needed == (kind == NF_VALUE_UTF8 ? 12u : 11u) && buffer[0] == 'x');
        CHECK(typed->result_read(f.runtime, f.b, result, &out, buffer, sizeof(buffer), &needed) ==
              NF_OK);
        CHECK(std::memcmp(buffer, "hello\0world", 11) == 0);
        if (kind == NF_VALUE_UTF8)
            CHECK(out.as.text.size == 11 && buffer[11] == '\0');
        else
            CHECK(out.as.bytes.size == 11);
        CHECK(typed->result_release(f.runtime, f.b, result) == NF_OK &&
              f.stats().retained_payload_bytes == 0);
    }
}
TEST(close_invalidates_owned_functions_and_results)
{
    Fixture f;
    auto fn = register_echo(f);
    auto in = value(NF_VALUE_U64);
    nf_token result = 0;
    CHECK(typed->function_invoke(f.runtime, f.b, fn, &in, 1, &result) == NF_OK);
    CHECK(core->context_close(f.runtime, f.a) == NF_OK);
    CHECK(core->context_release(f.runtime, f.a) == NF_OK);
    f.a = 0;
    nf_token later = 0;
    CHECK(typed->function_invoke(f.runtime, f.b, fn, &in, 1, &later) == NF_E_HANDLE);
    CHECK(f.stats().functions == 0 && f.stats().results == 1);
    CHECK(core->context_close(f.runtime, f.b) == NF_OK);
    CHECK(f.stats().results == 0);
}
TEST(invalid_values_and_output_structures_are_rejected)
{
    auto in = value(NF_VALUE_BOOL), out = value(NF_VALUE_U64);
    in.as.boolean = 2;
    out.as.u64 = 77;
    CHECK(typed->value_convert(&in, NF_VALUE_BOOL, &out) == NF_E_ARGUMENT && out.as.u64 == 77);
    in = value(NF_VALUE_UTF8);
    in.as.text = {"\xc0\xaf", 2};
    CHECK(typed->value_convert(&in, NF_VALUE_UTF8, &out) == NF_E_ARGUMENT);
    in.as.text = {nullptr, 1};
    CHECK(typed->value_convert(&in, NF_VALUE_UTF8, &out) == NF_E_ARGUMENT);
    in.as.text = {"x", 1048577};
    CHECK(typed->value_convert(&in, NF_VALUE_UTF8, &out) == NF_E_ARGUMENT);
    in = value(NF_VALUE_BYTES);
    in.as.bytes = {nullptr, 1};
    CHECK(typed->value_convert(&in, NF_VALUE_BYTES, &out) == NF_E_ARGUMENT);
    in = value(NF_VALUE_U64);
    in.as.u64 = 42;
    out.struct_size = 4;
    CHECK(typed->value_convert(&in, NF_VALUE_U64, &out) == NF_E_ARGUMENT && out.struct_size == 4 &&
          out.as.u64 == 77);
}
TEST(signature_validation_conflicts_and_wrong_thread)
{
    Fixture f;
    uint32_t kinds[33]{};
    nf_signature sig{sizeof(sig), NF_VALUE_U64, 1, 0, kinds};
    nf_token fn = 99;
    CHECK(typed->function_register(f.runtime, f.a, span("typed.bad"), &sig, echo, nullptr, &fn) ==
              NF_E_ARGUMENT &&
          !fn);
    sig.argument_count = 33;
    CHECK(typed->function_register(f.runtime, f.a, span("typed.bad"), &sig, echo, nullptr, &fn) ==
          NF_E_ARGUMENT);
    auto id = register_echo(f);
    uint32_t kind = NF_VALUE_U64;
    sig = {sizeof(sig), kind, 1, 0, &kind};
    CHECK(typed->function_register(f.runtime, f.b, span("typed.echo"), &sig, echo, nullptr, &fn) ==
          NF_E_CONFLICT);
    CHECK(f.stats().functions == 1);
    nf_status status = NF_OK;
    std::thread other(
        [&]
        {
            nf_typed_stats stats{};
            stats.struct_size = sizeof(stats);
            status = typed->stats(f.runtime, &stats);
        });
    other.join();
    CHECK(status == NF_E_THREAD);
    CHECK(typed->function_unregister(f.runtime, f.a, id) == NF_OK);
}
TEST(callback_failures_do_not_leak_results)
{
    Fixture f;
    nf_signature sig{sizeof(sig), NF_VALUE_U64, 0, 0, nullptr};
    nf_token fn = 0, result = 99;
    auto throws = +[](nf_context, nf_context, const nf_value *, uint32_t, nf_value *,
                      void *) -> nf_status { throw std::runtime_error("fixture"); };
    CHECK(typed->function_register(f.runtime, f.a, span("typed.throws"), &sig, throws, nullptr,
                                   &fn) == NF_OK);
    CHECK(typed->function_invoke(f.runtime, f.b, fn, nullptr, 0, &result) == NF_E_CALLBACK &&
          !result);
    auto bad =
        +[](nf_context, nf_context, const nf_value *, uint32_t, nf_value *out, void *) -> nf_status
    {
        out->kind = NF_VALUE_BOOL;
        out->as.boolean = 1;
        return NF_OK;
    };
    CHECK(typed->function_register(f.runtime, f.a, span("typed.badresult"), &sig, bad, nullptr,
                                   &fn) == NF_OK);
    CHECK(typed->function_invoke(f.runtime, f.b, fn, nullptr, 0, &result) == NF_E_CALLBACK &&
          !result);
    auto unknown = +[](nf_context, nf_context, const nf_value *, uint32_t, nf_value *,
                       void *) -> nf_status { return 999; };
    CHECK(typed->function_register(f.runtime, f.a, span("typed.unknownstatus"), &sig, unknown,
                                   nullptr, &fn) == NF_OK);
    CHECK(typed->function_invoke(f.runtime, f.b, fn, nullptr, 0, &result) == NF_E_CALLBACK &&
          !result);
    CHECK(f.stats().results == 0 && f.stats().retained_payload_bytes == 0);
}
TEST(provider_can_unregister_and_close_during_result_copy)
{
    Fixture f;
    struct State
    {
        Fixture *f;
        nf_token fn = 0;
        std::string text = "owned result";
    } st{&f};
    nf_signature sig{sizeof(sig), NF_VALUE_UTF8, 0, 0, nullptr};
    auto callback = +[](nf_context provider, nf_context, const nf_value *, uint32_t, nf_value *out,
                        void *u) -> nf_status
    {
        auto &x = *static_cast<State *>(u);
        CHECK(typed->function_unregister(x.f->runtime, provider, x.fn) == NF_OK);
        CHECK(core->context_close(x.f->runtime, provider) == NF_OK);
        CHECK(core->context_release(x.f->runtime, provider) == NF_E_BUSY);
        *out = value(NF_VALUE_UTF8);
        out->as.text = span(x.text);
        return NF_OK;
    };
    CHECK(typed->function_register(f.runtime, f.a, span("typed.closing"), &sig, callback, &st,
                                   &st.fn) == NF_OK);
    nf_token result = 0;
    CHECK(typed->function_invoke(f.runtime, f.b, st.fn, nullptr, 0, &result) == NF_OK);
    CHECK(core->context_release(f.runtime, f.a) == NF_OK);
    f.a = 0;
    st.text = "replaced";
    auto out = value(NF_VALUE_VOID);
    char buffer[32];
    uint64_t needed = 0;
    CHECK(typed->result_read(f.runtime, f.b, result, &out, buffer, sizeof(buffer), &needed) ==
          NF_OK);
    CHECK(std::string_view(out.as.text.data, out.as.text.size) == "owned result");
}
TEST(caller_shutdown_during_callback_cannot_create_an_orphan_result)
{
    Fixture f;
    nf_signature sig{sizeof(sig), NF_VALUE_VOID, 0, 0, nullptr};
    nf_token fn = 0;
    auto callback = +[](nf_context, nf_context caller, const nf_value *, uint32_t, nf_value *,
                        void *u) -> nf_status
    {
        auto &x = *static_cast<Fixture *>(u);
        CHECK(core->context_close(x.runtime, caller) == NF_OK);
        CHECK(core->context_release(x.runtime, caller) == NF_OK);
        x.b = 0;
        return NF_OK;
    };
    CHECK(typed->function_register(f.runtime, f.a, span("typed.callerclose"), &sig, callback, &f,
                                   &fn) == NF_OK);
    nf_function_info info{};
    info.struct_size = sizeof(info);
    CHECK(typed->function_resolve(f.runtime, f.b, span("typed.callerclose"), &info, nullptr, 0) ==
          NF_OK);
    nf_token result = 99;
    CHECK(typed->function_invoke(f.runtime, f.b, fn, nullptr, 0, &result) == NF_E_STATE && !result);
    CHECK(f.stats().results == 0);
}
TEST(native_reentry_is_bounded_and_recovers)
{
    Fixture f;
    struct State
    {
        Fixture *f;
        nf_token fn = 0;
        int calls = 0;
    } st{&f};
    nf_signature sig{sizeof(sig), NF_VALUE_VOID, 0, 0, nullptr};
    auto recursive = +[](nf_context, nf_context caller, const nf_value *, uint32_t, nf_value *,
                         void *u) -> nf_status
    {
        auto &x = *static_cast<State *>(u);
        if (++x.calls >= 64)
            return NF_OK;
        nf_token result = 0;
        auto status = typed->function_invoke(x.f->runtime, caller, x.fn, nullptr, 0, &result);
        if (status == NF_OK)
            CHECK(typed->result_release(x.f->runtime, caller, result) == NF_OK);
        return status;
    };
    CHECK(typed->function_register(f.runtime, f.a, span("typed.recursion"), &sig, recursive, &st,
                                   &st.fn) == NF_OK);
    nf_token result = 99;
    CHECK(typed->function_invoke(f.runtime, f.b, st.fn, nullptr, 0, &result) == NF_E_LIMIT &&
          !result);
    CHECK(st.calls == 16 && f.stats().results == 0);
    st.calls = 63;
    CHECK(typed->function_invoke(f.runtime, f.b, st.fn, nullptr, 0, &result) == NF_OK);
    CHECK(typed->result_release(f.runtime, f.b, result) == NF_OK);
}
TEST(result_payload_budget_is_accounted_and_reclaimed)
{
    Fixture f;
    auto fn = register_echo(f, NF_VALUE_BYTES);
    std::string bytes(1048576, 'x');
    auto input = value(NF_VALUE_BYTES);
    input.as.bytes = {bytes.data(), bytes.size()};
    std::vector<nf_token> results;
    for (int i = 0; i < 16; ++i)
    {
        nf_token result = 0;
        CHECK(typed->function_invoke(f.runtime, f.b, fn, &input, 1, &result) == NF_OK);
        results.push_back(result);
    }
    CHECK(f.stats().retained_payload_bytes == 16 * 1048576);
    nf_token overflow = 99;
    CHECK(typed->function_invoke(f.runtime, f.b, fn, &input, 1, &overflow) == NF_E_LIMIT &&
          !overflow);
    CHECK(typed->result_release(f.runtime, f.b, results.back()) == NF_OK);
    results.pop_back();
    CHECK(typed->function_invoke(f.runtime, f.b, fn, &input, 1, &overflow) == NF_OK);
    results.push_back(overflow);
    CHECK(core->context_close(f.runtime, f.b) == NF_OK);
    CHECK(f.stats().results == 0 && f.stats().retained_payload_bytes == 0);
}
TEST(typed_capabilities_do_not_advertise_engine_reflection)
{
    Fixture f;
    nf_capability cap{};
    cap.struct_size = sizeof(cap);
    CHECK(core->capability_query(f.runtime, span("nf.values"), &cap) == NF_OK && cap.version == 1 &&
          !cap.game_tested);
    CHECK(core->capability_query(f.runtime, span("nf.reflection.functions"), &cap) == NF_OK &&
          cap.version == 1);
    CHECK(core->capability_query(f.runtime, span("nf.engine.reflection"), &cap) ==
          NF_E_UNAVAILABLE);
}
TEST(empty_spans_and_void_results_have_precise_buffer_sizes)
{
    for (uint32_t kind : {NF_VALUE_UTF8, NF_VALUE_BYTES, NF_VALUE_VOID})
    {
        Fixture f;
        auto empty = +[](nf_context, nf_context, const nf_value *, uint32_t, nf_value *out,
                         void *user) -> nf_status
        {
            *out = value(*static_cast<uint32_t *>(user));
            return NF_OK;
        };
        nf_signature signature{sizeof(signature), kind, 0, 0, nullptr};
        nf_token fn = 0, result = 0;
        CHECK(typed->function_register(f.runtime, f.a, span("typed.empty"), &signature, empty,
                                       &kind, &fn) == NF_OK);
        CHECK(typed->function_invoke(f.runtime, f.b, fn, nullptr, 0, &result) == NF_OK);
        auto out = value(NF_VALUE_U64);
        out.as.u64 = 19;
        uint64_t needed = 99;
        auto status = typed->result_read(f.runtime, f.b, result, &out, nullptr, 0, &needed);
        if (kind == NF_VALUE_UTF8)
        {
            CHECK(status == NF_E_BUFFER && needed == 1 && out.as.u64 == 19);
            char buffer = 'x';
            CHECK(typed->result_read(f.runtime, f.b, result, &out, &buffer, 1, &needed) == NF_OK);
            CHECK(buffer == 0 && out.as.text.size == 0 && out.as.text.data == &buffer);
        }
        else
            CHECK(status == NF_OK && needed == 0 && out.kind == kind);
    }
}
TEST(signature_metadata_is_copied_and_foreign_handles_are_rejected)
{
    Fixture f, foreign;
    uint32_t kind = NF_VALUE_U64;
    nf_signature signature{sizeof(signature), kind, 1, 0, &kind};
    nf_token fn = 0, result = 99;
    std::string name = "typed.copied";
    CHECK(typed->function_register(f.runtime, f.a, span(name), &signature, echo, nullptr, &fn) ==
          NF_OK);
    name = "typed.changed";
    kind = NF_VALUE_BOOL;
    auto input = value(NF_VALUE_U64);
    input.as.u64 = 42;
    CHECK(typed->function_invoke(foreign.runtime, foreign.b, fn, &input, 1, &result) ==
              NF_E_HANDLE &&
          !result);
    CHECK(typed->function_invoke(f.runtime, f.b, fn, &input, 1, &result) == NF_OK);
    auto output = value(NF_VALUE_VOID);
    uint64_t needed = 0;
    CHECK(typed->result_read(foreign.runtime, foreign.b, result, &output, nullptr, 0, &needed) ==
          NF_E_HANDLE);
    CHECK(typed->result_read(f.runtime, f.b, result, &output, nullptr, 0, &needed) == NF_OK &&
          output.as.u64 == 42);
}
TEST(function_and_result_handle_limits_recover_after_cleanup)
{
    Fixture f;
    uint32_t kind = NF_VALUE_U64;
    nf_signature signature{sizeof(signature), kind, 1, 0, &kind};
    nf_token fn = 0, result = 0;
    for (int i = 0; i < 4096; ++i)
        CHECK(typed->function_register(f.runtime, f.a, span("typed.fn" + std::to_string(i)),
                                       &signature, echo, nullptr, &fn) == NF_OK);
    nf_token overflow = 99;
    CHECK(typed->function_register(f.runtime, f.a, span("typed.overflow"), &signature, echo,
                                   nullptr, &overflow) == NF_E_LIMIT &&
          !overflow);
    auto input = value(NF_VALUE_U64);
    for (int i = 0; i < 4096; ++i)
        CHECK(typed->function_invoke(f.runtime, f.b, fn, &input, 1, &result) == NF_OK);
    overflow = 99;
    CHECK(typed->function_invoke(f.runtime, f.b, fn, &input, 1, &overflow) == NF_E_LIMIT &&
          !overflow);
    CHECK(typed->result_release(f.runtime, f.b, result) == NF_OK);
    CHECK(typed->function_invoke(f.runtime, f.b, fn, &input, 1, &result) == NF_OK);
    CHECK(core->context_close(f.runtime, f.a) == NF_OK);
    CHECK(core->context_close(f.runtime, f.b) == NF_OK);
    CHECK(f.stats().functions == 0 && f.stats().results == 0);
}
int main()
{
    if (nf_get_api(1, sizeof(nf_api), &core) != NF_OK)
        return 1;
    auto module = GetModuleHandleW(L"NeonForge.Core.dll");
    get_typed = reinterpret_cast<GetTyped>(GetProcAddress(module, "nf_get_typed_api"));
    if (!get_typed)
    {
        std::cout << "FAIL typed extension is not implemented\n";
        return 1;
    }
    if (get_typed(1, sizeof(nf_typed_api), &typed) != NF_OK)
        return 1;
    return run_tests();
}
