#include "json.hpp"
#include "script_host.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>

namespace
{
using Clock = std::chrono::steady_clock;
nf_str span(std::string_view s) { return {s.data(), s.size()}; }
void require(bool ok)
{
    if (!ok)
        throw std::runtime_error("script benchmark work/cleanup verification failed");
}
uint32_t number(const char *text, uint32_t maximum)
{
    uint32_t value = 0;
    std::string_view s(text);
    auto parsed = std::from_chars(s.data(), s.data() + s.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size() || !value || value > maximum)
        throw std::runtime_error("invalid iterations/samples");
    return value;
}
struct Scratch
{
    std::filesystem::path path;
    Scratch()
    {
        path =
            std::filesystem::temp_directory_path() /
            ("neonforge-script-bench-" + std::to_string(Clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(path))
            throw std::runtime_error("benchmark scratch directory already exists");
    }
    ~Scratch()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
} // namespace
int main(int argc, char **argv)
{
    try
    {
        if (argc != 1 && argc != 3)
            throw std::runtime_error("usage: nf_script_benchmark [iterations samples]");
        uint32_t iterations = argc == 3 ? number(argv[1], 10000) : 250;
        uint32_t samples = argc == 3 ? number(argv[2], 31) : 7;
        const nf_api *core = nullptr;
        const nf_typed_api *typed = nullptr;
        require(nf_get_api(1, sizeof(nf_api), &core) == NF_OK &&
                nf_get_typed_api(1, sizeof(nf_typed_api), &typed) == NF_OK);
        Scratch scratch;
        std::ofstream(scratch.path / "init.lua")
            << "hits=0; nf.export('bench.echo','i64',{'i64'},function(x) hits=hits+1; return x "
               "end); nf.on('bench.event',function(payload) assert(payload=='x'); hits=hits+1 end)";
        nf::Json::Array cases;
        for (std::string name : {"script.typed_call", "script.event", "script.load_unload"})
        {
            std::vector<double> timings;
            uint64_t peak = 0;
            for (uint32_t sample = 0; sample <= samples; ++sample)
            {
                auto root = (scratch.path / "data").generic_string();
                nf_runtime_options options{sizeof(options), 0, span(root)};
                nf_runtime *runtime = nullptr;
                require(core->runtime_create(&options, &runtime) == NF_OK);
                try
                {
                    nf::ScriptHost host(core, typed, runtime);
                    require(host.load("bench.mod", scratch.path, "init.lua"));
                    std::string code;
                    if (name == "script.typed_call")
                        code = "total=0; for i=1," + std::to_string(iterations) +
                               " do total=total+nf.call('bench.echo',i) end";
                    else
                        code = "for i=1," + std::to_string(iterations) +
                               " do nf.emit('bench.event','x') end";
                    if (name == "script.load_unload")
                        require(host.unload("bench.mod"));
                    auto start = Clock::now();
                    if (name == "script.load_unload")
                    {
                        for (uint32_t i = 0; i < iterations; ++i)
                        {
                            require(host.load("bench.mod", scratch.path, "init.lua"));
                            require(host.stats("bench.mod").calls == 1 &&
                                    host.stats("bench.mod").bindings == 2);
                            peak = std::max(peak, host.stats("bench.mod").peak_bytes);
                            require(host.unload("bench.mod"));
                        }
                    }
                    else
                        require(host.execute("bench.mod", code));
                    auto elapsed =
                        std::chrono::duration<double, std::nano>(Clock::now() - start).count();
                    if (name != "script.load_unload")
                    {
                        require(host.execute("bench.mod",
                                             "assert(hits==" + std::to_string(iterations) + ")"));
                        if (name == "script.typed_call")
                            require(host.execute(
                                "bench.mod",
                                "assert(total==" +
                                    std::to_string(uint64_t(iterations) * (iterations + 1) / 2) +
                                    ")"));
                        peak = std::max(peak, host.stats("bench.mod").peak_bytes);
                        require(host.unload("bench.mod"));
                    }
                    nf_stats stats{sizeof(stats)};
                    nf_typed_stats typed_stats{sizeof(typed_stats)};
                    require(core->stats(runtime, &stats) == NF_OK && stats.contexts == 0 &&
                            stats.subscriptions == 0 && stats.queued_tasks == 0);
                    require(typed->stats(runtime, &typed_stats) == NF_OK &&
                            typed_stats.functions == 0 && typed_stats.results == 0 &&
                            typed_stats.retained_payload_bytes == 0);
                    if (sample)
                        timings.push_back(elapsed / iterations);
                }
                catch (...)
                {
                    core->runtime_destroy(runtime);
                    throw;
                }
                require(core->runtime_destroy(runtime) == NF_OK);
            }
            auto sorted = timings;
            std::sort(sorted.begin(), sorted.end());
            nf::Json::Array raw;
            for (double value : timings)
                raw.emplace_back(value);
            nf::Json::Object item;
            item.emplace("name", nf::Json(name));
            item.emplace("operations_per_sample", nf::Json(double(iterations)));
            item.emplace("verified_operations", nf::Json(double(uint64_t(iterations) * samples)));
            item.emplace("samples_ns_per_operation", nf::Json(raw));
            item.emplace(
                "median_ns_per_operation",
                nf::Json((sorted[(sorted.size() - 1) / 2] + sorted[sorted.size() / 2]) / 2));
            item.emplace("peak_lua_bytes", nf::Json(double(peak)));
            cases.emplace_back(item);
        }
        nf::Json::Object report;
        report.emplace("lua", nf::Json(std::string("5.5.1")));
        report.emplace("version", nf::Json(std::string("0.1.3-dev.1")));
        report.emplace("scope", nf::Json(std::string(
                                    "standalone Lua; no external-framework or game comparison")));
        report.emplace("cases", nf::Json(cases));
        std::cout << nf::to_json(nf::Json(report)) << '\n';
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "NEONFORGE Lua benchmark: " << e.what() << '\n';
        return 1;
    }
}
