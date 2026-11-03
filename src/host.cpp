#define NOMINMAX
#include "manifest.hpp"
#include "script_host.hpp"
#include <fstream>
#include <iostream>
#include <memory>
#include <neonforge/api.h>
#include <windows.h>

namespace
{
nf_str span(std::string_view x) { return {x.data(), x.size()}; }
std::string utf8(const std::filesystem::path &p)
{
    auto s = p.u8string();
    return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}
std::filesystem::path path(std::string_view s)
{
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}
void no_reparse(const std::filesystem::path &p)
{
    std::filesystem::path current;
    for (auto &part : std::filesystem::absolute(p))
    {
        current /= part;
        auto a = GetFileAttributesW(current.c_str());
        if (a == INVALID_FILE_ATTRIBUTES)
            throw std::runtime_error("cannot inspect path: " + utf8(current));
        if (a & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("reparse paths are not accepted: " + utf8(current));
    }
}
std::string read(const std::filesystem::path &p)
{
    no_reparse(p);
    if (std::filesystem::file_size(p) > 1048576)
        throw std::runtime_error("manifest exceeds 1 MiB");
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot read manifest");
    return {std::istreambuf_iterator<char>(f), {}};
}
std::vector<nf::Manifest> discover(const std::filesystem::path &root)
{
    no_reparse(root);
    if (!std::filesystem::is_directory(root))
        throw std::runtime_error("mods path must be a directory");
    std::vector<nf::Manifest> out;
    for (auto &entry : std::filesystem::directory_iterator(root))
    {
        if (entry.is_symlink())
            throw std::runtime_error("mod symlinks are not accepted");
        if (!entry.is_directory())
            continue;
        no_reparse(entry.path());
        auto file = entry.path() / "manifest.json";
        if (!std::filesystem::exists(file))
            continue;
        auto m = nf::parse_manifest(read(file), std::filesystem::absolute(entry.path()));
        auto dll = m.directory / path(m.entry);
        no_reparse(dll);
        if (!std::filesystem::is_regular_file(dll))
            throw std::runtime_error("entry is not a file: " + utf8(dll));
        out.push_back(std::move(m));
    }
    return out;
}
std::filesystem::path executable_dir()
{
    std::wstring buffer(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!n || n >= buffer.size())
        throw std::runtime_error("cannot resolve host executable");
    buffer.resize(n);
    return std::filesystem::path(buffer).parent_path();
}
struct Library
{
    HMODULE h = nullptr;
    explicit Library(const std::filesystem::path &p)
    {
        no_reparse(p);
        h = LoadLibraryExW(p.c_str(), nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!h)
            throw std::runtime_error("cannot load DLL: " + utf8(p) + " (Windows error " +
                                     std::to_string(GetLastError()) + ")");
    }
    ~Library()
    {
        if (h)
            FreeLibrary(h);
    }
    Library(const Library &) = delete;
    void *symbol(const char *n)
    {
        auto p = GetProcAddress(h, n);
        if (!p)
            throw std::runtime_error(std::string("missing export: ") + n);
        return reinterpret_cast<void *>(p);
    }
};
std::string error(const nf_api *a, nf_runtime *r)
{
    uint64_t n = 0;
    a->last_error(r, nullptr, 0, &n);
    std::string out(static_cast<size_t>(n), '\0');
    if (n)
        a->last_error(r, out.data(), out.size(), &n);
    if (!out.empty())
        out.pop_back();
    return out;
}
void check(nf_status s, std::string what, const nf_api *a = nullptr, nf_runtime *r = nullptr)
{
    if (s != NF_OK)
        throw std::runtime_error(what + " (status " + std::to_string(s) + ")" +
                                 (a && r ? ": " + error(a, r) : ""));
}
struct Mod
{
    const nf::Manifest *manifest;
    nf_context context = 0;
    std::unique_ptr<Library> library;
    nf_plugin_lifecycle lifecycle{sizeof(nf_plugin_lifecycle), 0, nullptr, nullptr};
    bool attempted = false;
    nf_plugin_start_fn start = nullptr;
};
nf::Json order_json(const std::vector<nf::Manifest> &mods, const std::vector<size_t> &order)
{
    nf::Json::Array names;
    for (auto i : order)
        names.emplace_back(mods[i].id);
    return nf::Json(names);
}
auto host_capabilities()
{
    auto capabilities = nf::core_capabilities();
    capabilities.emplace("nf.script.lua", 1);
    return capabilities;
}
int run(const std::vector<nf::Manifest> &mods, const std::vector<size_t> &order,
        const std::filesystem::path &data, uint32_t ticks)
{
    Library core(executable_dir() / "NeonForge.Core.dll");
    using GetApi = nf_status(NF_CALL *)(uint32_t, uint32_t, const nf_api **);
    auto get = reinterpret_cast<GetApi>(core.symbol("nf_get_api"));
    const nf_api *a = nullptr;
    check(get(1, sizeof(nf_api), &a), "runtime ABI negotiation");
    using GetTyped = nf_status(NF_CALL *)(uint32_t, uint32_t, const nf_typed_api **);
    const nf_typed_api *typed = nullptr;
    check(reinterpret_cast<GetTyped>(core.symbol("nf_get_typed_api"))(1, sizeof(nf_typed_api),
                                                                      &typed),
          "typed ABI negotiation");
    auto root = utf8(std::filesystem::absolute(data));
    nf_runtime_options options{sizeof(options), 0, span(root)};
    nf_runtime *r = nullptr;
    check(a->runtime_create(&options, &r), "runtime creation");
    auto scripts = std::make_unique<nf::ScriptHost>(a, typed, r);
    std::vector<Mod> loaded;
    loaded.reserve(order.size());
    nf_context host = 0;
    std::string failure;
    uint32_t started = 0;
    try
    {
        check(a->context_create(r, span("org.neonforge.host"), &host), "host context", a, r);
        for (auto i : order)
        {
            Mod m{&mods[i]};
            if (path(mods[i].entry).extension() == L".dll")
                check(a->context_create(r, span(mods[i].id), &m.context), "mod context", a, r);
            loaded.push_back(std::move(m));
        }
        for (auto &m : loaded)
        {
            if (path(m.manifest->entry).extension() == L".lua")
            {
                bool prepared =
                    scripts->prepare(m.manifest->id, m.manifest->directory, m.manifest->entry);
                if (!prepared)
                    throw std::runtime_error(scripts->error());
                m.context = scripts->context(m.manifest->id);
                continue;
            }
            m.library = std::make_unique<Library>(m.manifest->directory / path(m.manifest->entry));
            auto query = reinterpret_cast<nf_plugin_query_fn>(m.library->symbol("nf_plugin_query"));
            m.start = reinterpret_cast<nf_plugin_start_fn>(m.library->symbol("nf_plugin_start"));
            nf_plugin_info info{};
            info.struct_size = sizeof(info);
            check(query(&info), "plugin query");
            if (info.struct_size != sizeof(info) || info.abi_version != 1 || info.reserved ||
                !info.id.data || info.id.size > 128 ||
                std::string_view(info.id.data, static_cast<size_t>(info.id.size)) !=
                    m.manifest->id ||
                nf::Version{info.version_major, info.version_minor, info.version_patch} !=
                    m.manifest->version)
                throw std::runtime_error("plugin query does not match manifest: " + m.manifest->id);
        }
        // Every query and declared version is validated before the first start side effect.
        for (auto &m : loaded)
        {
            m.attempted = true;
            if (path(m.manifest->entry).extension() == L".lua")
            {
                if (!scripts->start(m.manifest->id))
                    throw std::runtime_error(scripts->error());
                ++started;
                continue;
            }
            check(m.start(a, r, m.context, &m.lifecycle), "plugin start: " + m.manifest->id, a, r);
            if (m.lifecycle.struct_size != sizeof(m.lifecycle) || m.lifecycle.reserved)
                throw std::runtime_error("plugin lifecycle ABI mismatch");
            ++started;
        }
        check(a->event_emit(r, host, span("nf.host.ready"), {}), "ready callbacks", a, r);
        for (uint32_t i = 0; i < ticks; ++i)
        {
            uint32_t ran = 0;
            check(a->task_pump(r, 4096, &ran), "task dispatch", a, r);
            if (!scripts->healthy())
                throw std::runtime_error("script callback failed; see attributed runtime log");
        }
    }
    catch (const std::exception &e)
    {
        failure = e.what();
    }
    catch (...)
    {
        failure = "plugin threw during its native entry point";
    }
    if (!scripts->healthy())
        failure = scripts->error();
    for (auto &m : loaded)
    {
        auto status = m.context ? a->context_close(r, m.context) : NF_OK;
        if (status != NF_OK && failure.empty())
            failure = "context close failed";
    }
    if (host)
        a->context_close(r, host);
    for (auto i = loaded.rbegin(); i != loaded.rend(); ++i)
    {
        if (path(i->manifest->entry).extension() == L".lua")
        {
            if (i->context)
                scripts->unload(i->manifest->id);
            i->context = 0;
            continue;
        }
        if (i->attempted && i->lifecycle.struct_size == sizeof(i->lifecycle) && i->lifecycle.stop)
        {
            try
            {
                i->lifecycle.stop(i->lifecycle.user);
            }
            catch (...)
            {
                if (failure.empty())
                    failure = "plugin stop threw";
            }
        }
    }
    scripts.reset();
    bool clean = true;
    for (auto &m : loaded)
        if (m.context && a->context_release(r, m.context) != NF_OK)
            clean = false;
    if (host && a->context_release(r, host) != NF_OK)
        clean = false;
    nf_stats stats{};
    stats.struct_size = sizeof(stats);
    check(a->stats(r, &stats), "final statistics", a, r);
    nf::Json::Object report;
    report.emplace("product", nf::Json(std::string("NEONFORGE Core")));
    report.emplace("version", nf::Json(std::string("0.1.8-dev.1")));
    report.emplace("mode", nf::Json(std::string("standalone; engine services unavailable")));
    report.emplace("started", nf::Json(static_cast<double>(started)));
    report.emplace("load_order", order_json(mods, order));
    report.emplace("remaining_contexts", nf::Json(static_cast<double>(stats.contexts)));
    report.emplace("remaining_tasks", nf::Json(static_cast<double>(stats.queued_tasks)));
    report.emplace("remaining_subscriptions", nf::Json(static_cast<double>(stats.subscriptions)));
    report.emplace("callbacks", nf::Json(static_cast<double>(stats.callbacks)));
    report.emplace("callback_nanoseconds", nf::Json(std::to_string(stats.callback_nanoseconds)));
    report.emplace("callback_errors", nf::Json(static_cast<double>(stats.callback_errors)));
    report.emplace("failure", nf::Json(failure));
    std::cout << nf::to_json(nf::Json(report)) << '\n';
    if (!clean)
    {
        std::cerr << "Unsafe to unload: runtime still owns native resources. Process will exit "
                     "without manual DLL unload.\n";
        for (auto &m : loaded)
            if (m.library)
                m.library->h = nullptr;
        core.h = nullptr;
        return 1;
    }
    check(a->runtime_destroy(r), "runtime destruction");
    if (!failure.empty())
    {
        std::cerr << failure << '\n';
        return 1;
    }
    return 0;
}
} // namespace
int wmain(int argc, wchar_t **wide)
{
    try
    {
        // Keep the current directory and PATH out of later DLL searches.
        if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
            throw std::runtime_error("cannot restrict DLL search directories");
        std::vector<std::string> args;
        for (int i = 0; i < argc; ++i)
            args.push_back(utf8(std::filesystem::path(wide[i])));
        if (argc == 2 && args[1] == "--version")
        {
            std::cout << "NEONFORGE Core 0.1.8-dev.1 (standalone SDK candidate)\n";
            return 0;
        }
        if (argc == 2 && args[1] == "capabilities")
        {
            nf::Json::Object c;
            for (auto &[n, v] : host_capabilities())
                c.emplace(n, nf::Json(static_cast<double>(v)));
            nf::Json::Object out;
            out.emplace("implemented", nf::Json(c));
            out.emplace("game_tested", nf::Json(false));
            out.emplace("engine_services", nf::Json(std::string("unavailable")));
            std::cout << nf::to_json(nf::Json(out)) << '\n';
            return 0;
        }
        if (argc < 3 || (args[1] != "validate" && args[1] != "run"))
        {
            std::cerr << "NEONFORGE Core\n  neonforge validate <mods-directory>\n  neonforge run "
                         "<mods-directory> <data-directory> [--ticks N]\n  neonforge "
                         "capabilities\n  neonforge --version\n";
            return 2;
        }
        auto mods = discover(path(args[2]));
        auto order = nf::resolve(mods, host_capabilities());
        if (args[1] == "validate")
        {
            if (argc != 3)
                throw std::runtime_error("unexpected validation arguments");
            nf::Json::Object o;
            o.emplace("valid", nf::Json(true));
            o.emplace("load_order", order_json(mods, order));
            std::cout << nf::to_json(nf::Json(o)) << '\n';
            return 0;
        }
        if (argc != 4 && argc != 6)
            throw std::runtime_error("run needs a data directory and optional --ticks N");
        uint32_t ticks = 1;
        if (argc == 6)
        {
            if (args[4] != "--ticks")
                throw std::runtime_error("expected --ticks");
            auto v = nf::Version::parse(args[5] + ".0.0");
            ticks = v.major;
            if (ticks > 100000)
                throw std::runtime_error("tick limit is 100000");
        }
        return run(mods, order, path(args[3]), ticks);
    }
    catch (const std::exception &e)
    {
        std::cerr << "NEONFORGE: " << e.what() << '\n';
        return 1;
    }
    catch (...)
    {
        std::cerr << "NEONFORGE: unexpected failure\n";
        return 1;
    }
}
