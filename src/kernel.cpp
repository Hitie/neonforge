#define NOMINMAX
#include "kernel.hpp"
#include "event_routes.hpp"
#include "manifest.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <windows.h>

namespace nf
{
struct Context
{
    std::string id;
    bool closing = false;
    uint32_t executing = 0, pins = 0;
    nf_context_stats metrics{};
};
struct Subscription
{
    nf_context owner;
    std::string name;
    nf_event_fn fn;
    void *user;
};
struct Task
{
    nf_context owner;
    nf_task_fn fn;
    void *user;
    nf_token token;
    Task *previous = nullptr, *next = nullptr;
    bool queued = false;
};
struct Service
{
    nf_context owner;
    std::string name;
    uint32_t version;
    const void *table;
    uint32_t size;
    bool visible = true;
};
struct Lease
{
    nf_context owner;
    nf_context provider;
    nf_token service;
};
struct Failure
{
    nf_status status;
    std::string text;
};
[[noreturn]] void fail(nf_status code, std::string text) { throw Failure{code, std::move(text)}; }
struct WinHandle
{
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit WinHandle(HANDLE v) : h(v) {}
    ~WinHandle()
    {
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    WinHandle(const WinHandle &) = delete;
    WinHandle &operator=(const WinHandle &) = delete;
};
std::string str(nf_str s, size_t max = 1048576)
{
    if (s.size > max || (!s.data && s.size))
        fail(NF_E_ARGUMENT, "invalid string span/length");
    std::string out(s.data ? s.data : "", static_cast<size_t>(s.size));
    if (!valid_utf8(out) || out.find('\0') != out.npos)
        fail(NF_E_ARGUMENT, "invalid UTF-8 or embedded NUL");
    return out;
}
nf_str span(const std::string &s) { return {s.data(), s.size()}; }
std::filesystem::path path_utf8(const std::string &s)
{
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}
void check_no_reparse(const std::filesystem::path &p)
{
    auto absolute = std::filesystem::absolute(p);
    std::filesystem::path current;
    for (auto &part : absolute)
    {
        current /= part;
        DWORD a = GetFileAttributesW(current.c_str());
        if (a == INVALID_FILE_ATTRIBUTES)
        {
            auto e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
                continue;
            fail(NF_E_IO, "cannot inspect storage path");
        }
        if (a & FILE_ATTRIBUTE_REPARSE_POINT)
            fail(NF_E_ARGUMENT, "storage path contains a reparse point");
    }
}
std::filesystem::path directory(const std::filesystem::path &root,
                                std::initializer_list<std::string> children)
{
    auto p = root;
    check_no_reparse(p);
    for (auto &s : children)
    {
        p /= path_utf8(s);
        check_no_reparse(p);
        std::filesystem::create_directory(p);
        check_no_reparse(p);
    }
    return p;
}
nf_status copy_string(std::string_view text, char *out, uint64_t capacity, uint64_t *needed)
{
    if (!needed)
        return NF_E_ARGUMENT;
    *needed = text.size() + 1;
    if (!out || capacity < *needed)
        return NF_E_BUFFER;
    std::memcpy(out, text.data(), text.size());
    out[text.size()] = 0;
    return NF_OK;
}
std::atomic<uint64_t> ids{1};
uint64_t token()
{
    auto n = ids.fetch_add(1, std::memory_order_relaxed);
    if (!n || n == UINT64_MAX)
        fail(NF_E_LIMIT, "handle space exhausted");
    return n;
}
} // namespace nf
struct nf_runtime
{
    std::thread::id thread = std::this_thread::get_id();
    std::filesystem::path root;
    std::map<nf_context, nf::Context> contexts;
    std::map<nf_token, nf::Subscription> subscriptions;
    nf::EventRoutes event_routes;
    std::map<nf_token, nf::Task> tasks;
    nf::Task *queue_head = nullptr, *queue_tail = nullptr;
    uint32_t queue_size = 0;
    std::vector<nf_token> task_snapshot;
    std::map<nf_token, nf::Service> services;
    std::map<nf_token, nf::Lease> leases;
    std::string error;
    nf_stats counters{};
    unsigned dispatch_depth = 0, active_calls = 0;
    bool pumping = false;
};
namespace nf
{
std::mutex registry_mutex;
// Public nf_runtime* values are unique opaque identities, never dereferenced object addresses.
// State allocations can be reclaimed without making an old identity valid again.
std::unordered_map<nf_runtime *, std::unique_ptr<nf_runtime>> runtimes;
Context &context(nf_runtime *r, nf_context c, bool allow_closing = false)
{
    auto i = r->contexts.find(c);
    if (i == r->contexts.end())
        fail(NF_E_HANDLE, "expired or foreign context");
    if (i->second.closing && !allow_closing)
        fail(NF_E_STATE, "context is closing");
    return i->second;
}
template <class F> nf_status guard(nf_runtime *r, F fn)
{
    {
        std::lock_guard lock(registry_mutex);
        auto entry = runtimes.find(r);
        if (!r || entry == runtimes.end())
            return NF_E_HANDLE;
        r = entry->second.get();
        if (r->thread != std::this_thread::get_id())
            return NF_E_THREAD;
    }
    struct Active
    {
        nf_runtime *r;
        Active(nf_runtime *x) : r(x) { ++r->active_calls; }
        ~Active() { --r->active_calls; }
    } active(r);
    try
    {
        return fn(r);
    }
    catch (const Failure &e)
    {
        r->error = e.text;
        return e.status;
    }
    catch (const ParseError &e)
    {
        r->error = "JSON offset " + std::to_string(e.offset) + ": " + e.what();
        return NF_E_ARGUMENT;
    }
    catch (const std::invalid_argument &e)
    {
        r->error = e.what();
        return NF_E_ARGUMENT;
    }
    catch (const std::filesystem::filesystem_error &e)
    {
        r->error = e.what();
        return NF_E_IO;
    }
    catch (const std::exception &e)
    {
        r->error = e.what();
        return NF_E_STATE;
    }
    catch (...)
    {
        r->error = "unexpected exception contained at ABI boundary";
        return NF_E_STATE;
    }
}
std::string name(nf_str text)
{
    auto n = str(text, 128);
    if (!valid_id(n))
        fail(NF_E_ARGUMENT, "name must use lower-case dotted identifiers");
    return n;
}
const std::map<std::string, uint32_t, std::less<>> &capabilities() { return core_capabilities(); }
nf_status NF_CALL create(const nf_runtime_options *o, nf_runtime **out)
{
    if (!out)
        return NF_E_ARGUMENT;
    *out = nullptr;
    if (!o || o->struct_size != sizeof(*o) || o->reserved)
        return NF_E_ARGUMENT;
    try
    {
        auto root = str(o->data_root, 32760);
        if (root.empty())
            return NF_E_ARGUMENT;
        auto p = std::filesystem::absolute(path_utf8(root)).lexically_normal();
        check_no_reparse(p);
        std::filesystem::create_directories(p);
        check_no_reparse(p);
        auto r = std::make_unique<nf_runtime>();
        r->root = p;
        static_assert(sizeof(uintptr_t) == sizeof(uint64_t), "NEONFORGE requires Windows x64");
        auto handle = reinterpret_cast<nf_runtime *>(static_cast<uintptr_t>(token()));
        {
            std::lock_guard lock(registry_mutex);
            runtimes.emplace(handle, std::move(r));
        }
        *out = handle;
        return NF_OK;
    }
    catch (const Failure &e)
    {
        return e.status;
    }
    catch (...)
    {
        return NF_E_IO;
    }
}
nf_status NF_CALL destroy(nf_runtime *r)
{
    std::lock_guard lock(registry_mutex);
    auto entry = runtimes.find(r);
    if (!r || entry == runtimes.end())
        return NF_E_HANDLE;
    auto state = entry->second.get();
    if (state->thread != std::this_thread::get_id())
        return NF_E_THREAD;
    if (!state->contexts.empty() || state->active_calls)
        return NF_E_BUSY;
    runtimes.erase(entry);
    return NF_OK;
}
nf_status NF_CALL context_create(nf_runtime *r, nf_str id, nf_context *out)
{
    if (out)
        *out = 0;
    return guard(r,
                 [&](nf_runtime *r) -> nf_status
                 {
                     if (!out)
                         fail(NF_E_ARGUMENT, "null context output");
                     auto n = name(id);
                     if (r->contexts.size() >= 1024)
                         fail(NF_E_LIMIT, "too many contexts");
                     for (auto &[k, c] : r->contexts)
                     {
                         (void)k;
                         if (c.id == n)
                             fail(NF_E_CONFLICT, "duplicate active mod ID");
                     }
                     auto t = token();
                     r->contexts.emplace(t, Context{n});
                     *out = t;
                     return NF_OK;
                 });
}
void erase_lease(nf_runtime *r, std::map<nf_token, Lease>::iterator i)
{
    auto provider = r->contexts.find(i->second.provider);
    if (provider != r->contexts.end() && provider->second.pins)
        --provider->second.pins;
    r->leases.erase(i);
}
void unlink_task(nf_runtime *r, Task &task)
{
    if (!task.queued)
        return;
    if (task.previous)
        task.previous->next = task.next;
    else
        r->queue_head = task.next;
    if (task.next)
        task.next->previous = task.previous;
    else
        r->queue_tail = task.previous;
    task.previous = task.next = nullptr;
    task.queued = false;
    --r->queue_size;
}
nf_status NF_CALL context_close(nf_runtime *r, nf_context c)
{
    return guard(r,
                 [&](nf_runtime *r) -> nf_status
                 {
                     auto &owner = context(r, c, true);
                     if (owner.closing)
                         return NF_OK;
                     owner.closing = true;
                     for (auto i = r->subscriptions.begin(); i != r->subscriptions.end();)
                         if (i->second.owner == c)
                         {
                             r->event_routes.remove(i->second.name, i->first);
                             i = r->subscriptions.erase(i);
                         }
                         else
                             ++i;
                     for (auto i = r->tasks.begin(); i != r->tasks.end();)
                         if (i->second.owner == c)
                         {
                             unlink_task(r, i->second);
                             i = r->tasks.erase(i);
                         }
                         else
                             ++i;
                     for (auto &[t, s] : r->services)
                     {
                         (void)t;
                         if (s.owner == c)
                             s.visible = false;
                     }
                     for (auto i = r->leases.begin(); i != r->leases.end();)
                         if (i->second.owner == c)
                         {
                             auto old = i++;
                             erase_lease(r, old);
                         }
                         else
                             ++i;
                     return NF_OK;
                 });
}
nf_status NF_CALL context_release(nf_runtime *r, nf_context c)
{
    return guard(r,
                 [&](nf_runtime *r) -> nf_status
                 {
                     auto &owner = context(r, c, true);
                     if (!owner.closing)
                         fail(NF_E_STATE, "close context before releasing");
                     if (owner.executing || owner.pins)
                         fail(NF_E_BUSY, "context has executing callbacks or service leases");
                     std::erase_if(r->services, [&](auto &v) { return v.second.owner == c; });
                     r->contexts.erase(c);
                     return NF_OK;
                 });
}
nf_status NF_CALL capability_query(nf_runtime *r, nf_str key, nf_capability *out)
{
    return guard(r,
                 [&](nf_runtime *) -> nf_status
                 {
                     if (!out || out->struct_size != sizeof(*out))
                         fail(NF_E_ARGUMENT, "capability struct size mismatch");
                     *out = {sizeof(*out), 0, 0, 0, 0, 0};
                     auto i = capabilities().find(name(key));
                     if (i == capabilities().end())
                         return NF_E_UNAVAILABLE;
                     out->version = i->second;
                     out->implemented = 1;
                     return NF_OK;
                 });
}
nf_status NF_CALL subscribe(nf_runtime *r, nf_context c, nf_str event, nf_event_fn fn, void *user,
                            nf_token *out)
{
    if (out)
        *out = 0;
    return guard(r,
                 [&](nf_runtime *r) -> nf_status
                 {
                     context(r, c);
                     if (!fn || !out)
                         fail(NF_E_ARGUMENT, "missing event callback/output");
                     if (r->subscriptions.size() >= 16384)
                         fail(NF_E_LIMIT, "subscription limit");
                     auto n = name(event);
                     auto t = token();
                     r->subscriptions.emplace(t, Subscription{c, n, fn, user});
                     try
                     {
                         r->event_routes.add(n, t);
                     }
                     catch (...)
                     {
                         r->subscriptions.erase(t);
                         throw;
                     }
                     *out = t;
                     return NF_OK;
                 });
}
nf_status NF_CALL unsubscribe(nf_runtime *r, nf_context c, nf_token t)
{
    return guard(r,
                 [&](nf_runtime *r) -> nf_status
                 {
                     context(r, c, true);
                     auto i = r->subscriptions.find(t);
                     if (i == r->subscriptions.end())
                         return NF_E_HANDLE;
                     if (i->second.owner != c)
                         return NF_E_OWNER;
                     r->event_routes.remove(i->second.name, t);
                     r->subscriptions.erase(i);
                     return NF_OK;
                 });
}
template <class F> bool invoke(nf_runtime *r, nf_context c, F fn)
{
    auto &owner = context(r, c);
    ++owner.executing;
    auto start = std::chrono::steady_clock::now();
    bool ok = true;
    try
    {
        fn();
    }
    catch (...)
    {
        ok = false;
        ++r->counters.callback_errors;
        ++owner.metrics.callback_errors;
        r->error = "plugin callback threw; callback boundary contained the C++ exception";
    }
    auto elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - start)
                                             .count());
    r->counters.callback_nanoseconds += elapsed;
    owner.metrics.callback_nanoseconds += elapsed;
    ++owner.metrics.callbacks;
    ++r->counters.callbacks;
    --context(r, c, true).executing;
    return ok;
}
nf_status NF_CALL emit(nf_runtime *r, nf_context c, nf_str event, nf_bytes payload)
{
    return guard(r,
                 [&](nf_runtime *r) -> nf_status
                 {
                     context(r, c);
                     auto n = name(event);
                     if (payload.size > 1048576 || (!payload.data && payload.size))
                         fail(NF_E_ARGUMENT, "event payload exceeds limit or is null");
                     if (r->dispatch_depth >= 16)
                         fail(NF_E_LIMIT, "nested event dispatch exceeds 16");
                     struct Depth
                     {
                         unsigned &n;
                         Depth(unsigned &x) : n(x) { ++n; }
                         ~Depth() { --n; }
                     } depth(r->dispatch_depth);
                     auto snapshot = r->event_routes.snapshot(n);
                     if (!snapshot)
                         return NF_OK;
                     nf_event e{span(n), payload, c};
                     bool ok = true;
                     for (auto t : *snapshot)
                     {
                         auto i = r->subscriptions.find(t);
                         if (i == r->subscriptions.end())
                             continue;
                         auto owner = i->second.owner;
                         auto fn = i->second.fn;
                         auto user = i->second.user;
                         if (!invoke(r, owner, [&] { fn(owner, &e, user); }))
                             ok = false;
                     }
                     return ok ? NF_OK : NF_E_CALLBACK;
                 });
}
nf_status NF_CALL enqueue(nf_runtime *r, nf_context c, nf_task_fn fn, void *user, nf_token *out)
{
    if (out)
        *out = 0;
    return guard(r,
                 [&](nf_runtime *r) -> nf_status
                 {
                     context(r, c);
                     if (!fn || !out)
                         fail(NF_E_ARGUMENT, "missing task callback/output");
                     if (r->tasks.size() >= 4096)
                         fail(NF_E_LIMIT, "task limit");
                     auto t = token();
                     auto &task = r->tasks.emplace(t, Task{c, fn, user, t}).first->second;
                     task.previous = r->queue_tail;
                     task.queued = true;
                     if (r->queue_tail)
                         r->queue_tail->next = &task;
                     else
                         r->queue_head = &task;
                     r->queue_tail = &task;
                     ++r->queue_size;
                     *out = t;
                     return NF_OK;
                 });
}
nf_status NF_CALL cancel(nf_runtime *r, nf_context c, nf_token t)
{
    return guard(r,
                 [&](nf_runtime *r) -> nf_status
                 {
                     context(r, c, true);
                     auto i = r->tasks.find(t);
                     if (i == r->tasks.end())
                         return NF_E_HANDLE;
                     if (i->second.owner != c)
                         return NF_E_OWNER;
                     unlink_task(r, i->second);
                     r->tasks.erase(i);
                     return NF_OK;
                 });
}
nf_status NF_CALL pump(nf_runtime *r, uint32_t maximum, uint32_t *ran)
{
    if (ran)
        *ran = 0;
    return guard(r,
                 [&](nf_runtime *r) -> nf_status
                 {
                     if (!ran || maximum > 4096)
                         fail(NF_E_ARGUMENT, "invalid task pump limit/output");
                     if (r->pumping)
                         fail(NF_E_STATE, "task pump is not reentrant");
                     struct Pump
                     {
                         bool &v;
                         Pump(bool &x) : v(x) { v = true; }
                         ~Pump() { v = false; }
                     } p(r->pumping);
                     auto &snapshot = r->task_snapshot;
                     snapshot.clear();
                     snapshot.reserve(maximum);
                     for (size_t i = 0, n = std::min<size_t>(maximum, r->queue_size); i < n; ++i)
                     {
                         auto &task = *r->queue_head;
                         snapshot.push_back(task.token);
                         unlink_task(r, task);
                     }
                     bool ok = true;
                     for (auto t : snapshot)
                     {
                         auto i = r->tasks.find(t);
                         if (i == r->tasks.end())
                             continue;
                         auto task = i->second;
                         r->tasks.erase(i);
                         if (!invoke(r, task.owner, [&] { task.fn(task.owner, task.user); }))
                             ok = false;
                         ++*ran;
                         ++r->counters.tasks_run;
                     }
                     return ok ? NF_OK : NF_E_CALLBACK;
                 });
}
nf_status NF_CALL publish(nf_runtime *r, nf_context c, nf_str key, uint32_t version,
                          const void *table, uint32_t size, nf_token *out)
{
    if (out)
        *out = 0;
    return guard(r,
                 [&](nf_runtime *r) -> nf_status
                 {
                     context(r, c);
                     auto n = name(key);
                     if (!out || !table || !size || !version || size > 1048576)
                         fail(NF_E_ARGUMENT, "invalid service table");
                     if (r->services.size() >= 4096)
                         fail(NF_E_LIMIT, "service limit");
                     for (auto &[t, s] : r->services)
                     {
                         (void)t;
                         if (s.visible && s.name == n)
                             fail(NF_E_CONFLICT, "service already published: " + n);
                     }
                     auto t = token();
                     r->services.emplace(t, Service{c, n, version, table, size});
                     *out = t;
                     return NF_OK;
                 });
}
nf_status NF_CALL acquire(nf_runtime *r, nf_context c, nf_str key, uint32_t version, uint32_t size,
                          nf_service *out)
{
    if (out)
        *out = {};
    return guard(r,
                 [&](nf_runtime *r) -> nf_status
                 {
                     context(r, c);
                     if (!out || !version || !size)
                         fail(NF_E_ARGUMENT, "invalid service requirement");
                     auto n = name(key);
                     if (r->leases.size() >= 16384)
                         fail(NF_E_LIMIT, "service lease limit");
                     for (auto &[t, s] : r->services)
                         if (s.visible && s.name == n)
                         {
                             if (s.version < version || s.size < size)
                                 return NF_E_ABI;
                             auto lease = token();
                             r->leases.emplace(lease, Lease{c, s.owner, t});
                             ++context(r, s.owner).pins;
                             *out = {lease, s.table, s.size, s.version};
                             return NF_OK;
                         }
                     return NF_E_UNAVAILABLE;
                 });
}
nf_status NF_CALL release_service(nf_runtime *r, nf_context c, nf_token t)
{
    return guard(r,
                 [&](nf_runtime *r) -> nf_status
                 {
                     context(r, c, true);
                     auto i = r->leases.find(t);
                     if (i == r->leases.end())
                         return NF_E_HANDLE;
                     if (i->second.owner != c)
                         return NF_E_OWNER;
                     erase_lease(r, i);
                     return NF_OK;
                 });
}
std::string setting_key(nf_str key)
{
    auto n = str(key, 64);
    if (n.empty())
        fail(NF_E_ARGUMENT, "empty settings key");
    for (char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-'))
            fail(NF_E_ARGUMENT, "settings key must be alphanumeric, underscore or hyphen");
    return n;
}
nf_status NF_CALL write_settings(nf_runtime *r, nf_context c, nf_str key, nf_str data)
{
    return guard(
        r,
        [&](nf_runtime *r) -> nf_status
        {
            auto id = context(r, c).id;
            auto k = setting_key(key);
            auto value = str(data);
            parse_json(value); // Validate without altering numeric lexemes or expanding text.
            auto dir = directory(r->root, {"settings", id});
            auto target = dir / (k + ".json");
            check_no_reparse(target);
            auto temp = dir / (k + ".tmp-" + std::to_string(GetCurrentProcessId()) + '-' +
                               std::to_string(token()));
            WinHandle h(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (h.h == INVALID_HANDLE_VALUE)
                fail(NF_E_IO, "cannot create exclusive settings temporary file");
            bool success = false;
            try
            {
                DWORD written = 0;
                if (!WriteFile(h.h, value.data(), static_cast<DWORD>(value.size()), &written,
                               nullptr) ||
                    written != value.size() || !FlushFileBuffers(h.h))
                    fail(NF_E_IO, "cannot flush settings");
                CloseHandle(h.h);
                h.h = INVALID_HANDLE_VALUE;
                check_no_reparse(target);
                if (!MoveFileExW(temp.c_str(), target.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                    fail(NF_E_IO, "cannot atomically replace settings");
                success = true;
            }
            catch (...)
            {
                if (h.h != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(h.h);
                    h.h = INVALID_HANDLE_VALUE;
                }
                DeleteFileW(temp.c_str());
                throw;
            }
            (void)success;
            return NF_OK;
        });
}
nf_status NF_CALL read_settings(nf_runtime *r, nf_context c, nf_str key, char *out,
                                uint64_t capacity, uint64_t *needed)
{
    if (needed)
        *needed = 0;
    return guard(
        r,
        [&](nf_runtime *r) -> nf_status
        {
            auto id = context(r, c).id;
            auto k = setting_key(key);
            if (!needed)
                fail(NF_E_ARGUMENT, "missing settings size output");
            auto target = r->root / "settings" / id / (k + ".json");
            check_no_reparse(target);
            WinHandle h(CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (h.h == INVALID_HANDLE_VALUE)
            {
                if (GetLastError() == ERROR_FILE_NOT_FOUND ||
                    GetLastError() == ERROR_PATH_NOT_FOUND)
                    return NF_E_NOT_FOUND;
                return NF_E_IO;
            }
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(h.h, &info) ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
                return NF_E_ARGUMENT;
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(h.h, &size) || size.QuadPart > 1048576 || size.QuadPart < 0)
                return NF_E_LIMIT;
            std::string value(static_cast<size_t>(size.QuadPart), '\0');
            DWORD got = 0;
            if (!ReadFile(h.h, value.data(), static_cast<DWORD>(value.size()), &got, nullptr) ||
                got != value.size())
                return NF_E_IO;
            parse_json(value);
            return copy_string(value, out, capacity, needed);
        });
}
nf_status NF_CALL log(nf_runtime *r, nf_context c, uint32_t level, nf_str message)
{
    return guard(
        r,
        [&](nf_runtime *r) -> nf_status
        {
            auto id = context(r, c, true).id;
            if (level > 3)
                fail(NF_E_ARGUMENT, "log level outside 0..3");
            auto text = str(message, 16384);
            auto dir = directory(r->root, {"logs"});
            auto file = dir / "neonforge.jsonl";
            check_no_reparse(file);
            if (std::filesystem::exists(file) && std::filesystem::file_size(file) >= 1048576)
            {
                auto oldest = dir / "neonforge.4.jsonl";
                check_no_reparse(oldest);
                if (std::filesystem::exists(oldest))
                    std::filesystem::remove(oldest);
                for (int i = 3; i >= 0; --i)
                {
                    auto from = i ? dir / ("neonforge." + std::to_string(i) + ".jsonl") : file;
                    auto to = dir / ("neonforge." + std::to_string(i + 1) + ".jsonl");
                    check_no_reparse(from);
                    check_no_reparse(to);
                    if (std::filesystem::exists(from))
                        std::filesystem::rename(from, to);
                }
            }
            Json::Object row;
            row.emplace("mod", Json(id));
            row.emplace("level", Json(static_cast<double>(level)));
            row.emplace("message", Json(text));
            row.emplace("sequence", Json(std::to_string(r->counters.log_lines + 1)));
            WinHandle f(CreateFileW(file.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (f.h == INVALID_HANDLE_VALUE)
                fail(NF_E_IO, "cannot open log");
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(f.h, &info) || info.nNumberOfLinks != 1 ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
                fail(NF_E_ARGUMENT, "log must be an owned regular file without hardlinks");
            auto line = to_json(Json(row)) + '\n';
            DWORD written = 0;
            if (!WriteFile(f.h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) ||
                written != line.size() || !FlushFileBuffers(f.h))
                fail(NF_E_IO, "cannot flush log");
            ++r->counters.log_lines;
            return NF_OK;
        });
}
nf_status NF_CALL stats(nf_runtime *r, nf_stats *out)
{
    return guard(r,
                 [&](nf_runtime *r) -> nf_status
                 {
                     if (!out || out->struct_size != sizeof(*out))
                         fail(NF_E_ARGUMENT, "stats struct size mismatch");
                     *out = r->counters;
                     out->struct_size = sizeof(*out);
                     out->contexts = static_cast<uint32_t>(r->contexts.size());
                     out->subscriptions = static_cast<uint32_t>(r->subscriptions.size());
                     out->queued_tasks = static_cast<uint32_t>(r->tasks.size());
                     out->services = static_cast<uint32_t>(r->services.size());
                     out->leases = static_cast<uint32_t>(r->leases.size());
                     return NF_OK;
                 });
}
nf_status NF_CALL last_error(nf_runtime *r, char *out, uint64_t capacity, uint64_t *needed)
{
    return guard(r, [&](nf_runtime *r) -> nf_status
                 { return copy_string(r->error, out, capacity, needed); });
}
nf_status NF_CALL context_metrics(nf_runtime *r, nf_context c, nf_context_stats *out)
{
    return guard(r,
                 [&](nf_runtime *r) -> nf_status
                 {
                     auto &owner = context(r, c, true);
                     if (!out || out->struct_size != sizeof(*out))
                         fail(NF_E_ARGUMENT, "context stats struct size mismatch");
                     *out = owner.metrics;
                     out->struct_size = sizeof(*out);
                     out->closing = owner.closing ? 1u : 0u;
                     for (auto &[t, s] : r->subscriptions)
                     {
                         (void)t;
                         if (s.owner == c)
                             ++out->subscriptions;
                     }
                     for (auto &[t, task] : r->tasks)
                     {
                         (void)t;
                         if (task.owner == c)
                             ++out->queued_tasks;
                     }
                     for (auto &[t, s] : r->services)
                     {
                         (void)t;
                         if (s.owner == c)
                             ++out->services;
                     }
                     for (auto &[t, lease] : r->leases)
                     {
                         (void)t;
                         if (lease.owner == c)
                             ++out->leases;
                     }
                     return NF_OK;
                 });
}
const nf_api &kernel_api()
{
    static const nf_api api = {sizeof(nf_api),
                               NF_ABI_VERSION,
                               0,
                               1,
                               1,
                               0,
                               create,
                               destroy,
                               context_create,
                               context_close,
                               context_release,
                               capability_query,
                               subscribe,
                               unsubscribe,
                               emit,
                               enqueue,
                               cancel,
                               pump,
                               publish,
                               acquire,
                               release_service,
                               write_settings,
                               read_settings,
                               log,
                               stats,
                               last_error,
                               context_metrics};
    return api;
}
} // namespace nf
