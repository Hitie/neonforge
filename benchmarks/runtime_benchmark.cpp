#define NOMINMAX
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <neonforge/api.h>
#include <stdexcept>
#include <windows.h>
#ifdef NF_BENCHMARK_RELEASE
#define NF_BENCHMARK_CONFIG "Release"
#endif
#ifndef NF_BENCHMARK_CONFIG
#define NF_BENCHMARK_CONFIG "Unspecified"
#endif

namespace
{
using Clock = std::chrono::steady_clock;
nf_str span(std::string_view v) { return {v.data(), v.size()}; }
void require(nf_status status)
{
    if (status != NF_OK)
        throw std::runtime_error("runtime operation failed: " + std::to_string(status));
}
uint32_t argument(const wchar_t *text, uint32_t minimum, uint32_t maximum)
{
    std::wstring_view input(text);
    uint64_t result = 0;
    if (input.empty())
        throw std::runtime_error("empty numeric argument");
    for (auto c : input)
    {
        if (c < L'0' || c > L'9' || result > maximum)
            throw std::runtime_error("invalid numeric argument");
        result = result * 10 + static_cast<unsigned>(c - L'0');
    }
    if (result < minimum || result > maximum)
        throw std::runtime_error("numeric argument outside supported bounds");
    return static_cast<uint32_t>(result);
}
struct Module
{
    HMODULE handle;
    explicit Module(const std::filesystem::path &path)
        : handle(
              LoadLibraryExW(std::filesystem::absolute(path).c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
    {
        if (!handle)
            throw std::runtime_error("cannot load selected runtime DLL");
    }
    ~Module() { FreeLibrary(handle); }
    Module(const Module &) = delete;
    Module &operator=(const Module &) = delete;
};
struct Runtime
{
    const nf_api *api;
    nf_runtime *runtime = nullptr;
    nf_context listener = 0, sender = 0;
    Runtime(const nf_api *table, const std::filesystem::path &root) : api(table)
    {
        auto u = std::filesystem::absolute(root).u8string();
        std::string path(reinterpret_cast<const char *>(u.data()), u.size());
        nf_runtime_options options{sizeof(options), 0, span(path)};
        require(api->runtime_create(&options, &runtime));
        try
        {
            require(api->context_create(runtime, span("bench.listener"), &listener));
            require(api->context_create(runtime, span("bench.sender"), &sender));
        }
        catch (...)
        {
            cleanup();
            throw;
        }
    }
    bool cleanup()
    {
        if (!runtime)
            return true;
        bool ok = true;
        for (auto owner : {listener, sender})
            if (owner)
            {
                ok = api->context_close(runtime, owner) == NF_OK && ok;
                ok = api->context_release(runtime, owner) == NF_OK && ok;
            }
        nf_stats stats{};
        stats.struct_size = sizeof(stats);
        ok = api->stats(runtime, &stats) == NF_OK && ok;
        ok = !stats.contexts && !stats.subscriptions && !stats.queued_tasks && !stats.services &&
             !stats.leases && ok;
        ok = api->runtime_destroy(runtime) == NF_OK && ok;
        runtime = nullptr;
        return ok;
    }
    ~Runtime() { cleanup(); }
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;
};
void NF_CALL on_event(nf_context, const nf_event *, void *user)
{
    ++*static_cast<uint64_t *>(user);
}
void NF_CALL on_task(nf_context, void *user) { ++*static_cast<uint64_t *>(user); }
struct Workload
{
    const char *name;
    const char *operation;
    uint32_t listeners, unrelated;
};
nf::Json run_case(const Workload &work, const nf_api *api, const std::filesystem::path &root,
                  uint32_t iterations, uint32_t samples)
{
    std::vector<double> times;
    uint64_t callbacks = 0;
    const bool events = std::string_view(work.operation) == "event_emit";
    const bool cancel = std::string_view(work.operation) == "task_cancel";
    uint32_t operations = events ? iterations : 4096;
    for (uint32_t sample = 0; sample <= samples; ++sample)
    {
        Runtime state(api, root);
        uint64_t count = 0;
        nf_token token = 0;
        std::vector<nf_token> tasks;
        for (uint32_t i = 0; i < work.unrelated; ++i)
        {
            auto name = "bench.other.n" + std::to_string(i);
            require(api->event_subscribe(state.runtime, state.listener, span(name), on_event,
                                         &count, &token));
        }
        for (uint32_t i = 0; i < work.listeners; ++i)
            require(api->event_subscribe(state.runtime, state.listener, span("bench.tick"),
                                         on_event, &count, &token));
        if (!events)
            for (uint32_t i = 0; i < operations; ++i)
            {
                require(api->task_enqueue(state.runtime, state.listener, on_task, &count, &token));
                tasks.push_back(token);
            }
        // Prime route caches and code paths outside the measured interval for steady-state emit.
        if (events)
        {
            require(api->event_emit(state.runtime, state.sender, span("bench.tick"), {}));
            count = 0;
        }
        const auto start = Clock::now();
        if (events)
            for (uint32_t i = 0; i < iterations; ++i)
                require(api->event_emit(state.runtime, state.sender, span("bench.tick"), {}));
        else if (cancel)
            for (auto task : tasks)
                require(api->task_cancel(state.runtime, state.listener, task));
        else
        {
            uint32_t completed = 0;
            while (completed < operations)
            {
                uint32_t ran = 0;
                require(api->task_pump(state.runtime, 64, &ran));
                if (!ran)
                    throw std::runtime_error("task pump stopped before the workload completed");
                completed += ran;
            }
            if (completed != operations)
                throw std::runtime_error("task pump exceeded the workload");
        }
        const auto elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        callbacks = events ? uint64_t(iterations) * work.listeners : (cancel ? 0 : operations);
        if (count != callbacks)
            throw std::runtime_error("benchmark callback count mismatch");
        nf_stats pending{};
        pending.struct_size = sizeof(pending);
        require(api->stats(state.runtime, &pending));
        if (pending.queued_tasks)
            throw std::runtime_error("benchmark workload left pending tasks");
        if (!state.cleanup())
            throw std::runtime_error("benchmark resource cleanup failed");
        if (sample)
            times.push_back(elapsed / operations);
    }
    nf::Json::Array raw;
    for (auto time : times)
        raw.emplace_back(time);
    std::sort(times.begin(), times.end());
    return nf::Json(nf::Json::Object{
        {"name", nf::Json(std::string(work.name))},
        {"operation", nf::Json(std::string(work.operation))},
        {"matching_listeners", nf::Json(double(work.listeners))},
        {"unrelated_listeners", nf::Json(double(work.unrelated))},
        {"operations_per_sample", nf::Json(double(operations))},
        {"verified_callbacks_per_sample", nf::Json(double(callbacks))},
        {"cleanup_verified", nf::Json(true)},
        {"samples_ns_per_operation", nf::Json(std::move(raw))},
        {"min_ns_per_operation", nf::Json(times.front())},
        {"median_ns_per_operation", nf::Json(times[times.size() / 2])},
        {"p95_ns_per_operation", nf::Json(times[(times.size() * 95 + 99) / 100 - 1])}});
}
} // namespace
int wmain(int argc, wchar_t **argv)
{
    try
    {
        if (argc != 5)
            throw std::runtime_error(
                "usage: nf_benchmark RUNTIME_DLL DATA_ROOT ITERATIONS SAMPLES");
        const auto iterations = argument(argv[3], 1, 1000000);
        const auto samples = argument(argv[4], 3, 101);
        if (!(samples & 1))
            throw std::runtime_error("sample count must be odd");
        Module module(argv[1]);
        using GetApi = nf_status(NF_CALL *)(uint32_t, uint32_t, const nf_api **);
        auto get_api = reinterpret_cast<GetApi>(GetProcAddress(module.handle, "nf_get_api"));
        if (!get_api)
            throw std::runtime_error("runtime has no nf_get_api export");
        const nf_api *api = nullptr;
        require(get_api(NF_ABI_VERSION, sizeof(nf_api), &api));
        if (!api || api->struct_size != sizeof(nf_api) || api->abi_version != NF_ABI_VERSION)
            throw std::runtime_error("runtime returned an incompatible ABI");
        const Workload workloads[] = {{"event.empty", "event_emit", 0, 0},
                                      {"event.one", "event_emit", 1, 0},
                                      {"event.one.unrelated1024", "event_emit", 1, 1024},
                                      {"event.one.unrelated8192", "event_emit", 1, 8192},
                                      {"event.fanout64", "event_emit", 64, 0},
                                      {"event.fanout64.unrelated8192", "event_emit", 64, 8192},
                                      {"event.missing.unrelated8192", "event_emit", 0, 8192},
                                      {"task.cancel4096", "task_cancel", 0, 0},
                                      {"task.pump4096.batch64", "task_run", 0, 0}};
        nf::Json::Array cases;
        for (const auto &work : workloads)
            cases.push_back(run_case(work, api, argv[2], iterations, samples));
        nf::Json report(
            nf::Json::Object{{"schema_version", nf::Json(1.0)},
                             {"iterations", nf::Json(double(iterations))},
                             {"samples", nf::Json(double(samples))},
                             {"warmup_samples", nf::Json(1.0)},
                             {"configuration", nf::Json(std::string(NF_BENCHMARK_CONFIG))},
                             {"compiler_msvc", nf::Json(double(_MSC_FULL_VER))},
                             {"runtime_version", nf::Json(std::to_string(api->version_major) + "." +
                                                          std::to_string(api->version_minor) + "." +
                                                          std::to_string(api->version_patch))},
                             {"clock", nf::Json(std::string("std::chrono::steady_clock"))},
                             {"cases", nf::Json(std::move(cases))}});
        std::cout << nf::to_json(report) << '\n';
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "NEONFORGE benchmark: " << error.what() << '\n';
        return 1;
    }
}
