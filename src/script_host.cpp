#define NOMINMAX
#include "script_host.hpp"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "manifest.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <stdexcept>
#include <thread>
#include <windows.h>

namespace nf
{
namespace
{
nf_str span(std::string_view s) { return {s.data(), s.size()}; }
std::string diagnostic(std::string_view id, std::string_view input)
{
    std::string result = std::string(id) + ": ";
    bool unicode = valid_utf8(input);
    constexpr char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < input.size();)
    {
        unsigned char c = static_cast<unsigned char>(input[i]);
        size_t width = 1;
        std::string piece;
        if (c < 32 || c == 127 || (!unicode && c >= 128))
        {
            piece = "\\x00";
            piece[2] = digits[c >> 4];
            piece[3] = digits[c & 15];
        }
        else
        {
            if (c >= 128)
                width = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            piece.assign(input.substr(i, width));
        }
        if (result.size() + piece.size() > 16381)
        {
            result += "...";
            break;
        }
        result += piece;
        i += width;
    }
    return result;
}
std::string_view lua_error_text(lua_State *L)
{
    if (lua_type(L, -1) != LUA_TSTRING)
        return "non-string Lua error";
    size_t size = 0;
    const char *text = lua_tolstring(L, -1, &size);
    return {text, size};
}
std::string read_source(const std::filesystem::path &root, std::string_view relative)
{
    if (!safe_relative_path(relative))
        throw std::runtime_error("invalid module path");
    auto unicode =
        std::u8string(reinterpret_cast<const char8_t *>(relative.data()), relative.size());
    auto path = std::filesystem::absolute(root / std::filesystem::path(unicode));
    std::filesystem::path current;
    for (const auto &part : path)
    {
        current /= part;
        auto attrs = GetFileAttributesW(current.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("missing or linked script path");
    }
    auto size = std::filesystem::file_size(path);
    if (!std::filesystem::is_regular_file(path) || size > 1048576)
        throw std::runtime_error("script must be a regular text file of at most 1 MiB");
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("cannot read script");
    std::string source(static_cast<size_t>(size) + 1, '\0');
    file.read(source.data(), static_cast<std::streamsize>(source.size()));
    source.resize(static_cast<size_t>(file.gcount()));
    if (source.size() > size)
        throw std::runtime_error("script changed while reading");
    return source;
}
struct Script;
struct Frame
{
    Script *script;
    uint64_t remaining;
    bool exhausted = false;
};
struct Binding
{
    Script *script;
    int ref = LUA_NOREF;
    nf_token token = 0;
    uint32_t result_kind = NF_VALUE_VOID;
    std::vector<uint32_t> arguments;
    std::string returned;
};
struct Script
{
    const nf_api *api;
    const nf_typed_api *typed;
    nf_runtime *runtime;
    ScriptLimits limits;
    std::string id, entry, last_error;
    std::string &host_error;
    std::filesystem::path root;
    nf_context context = 0;
    lua_State *state = nullptr;
    Frame main_frame{this, 0};
    ScriptStats metrics;
    uint32_t depth = 0;
    int entry_ref = LUA_NOREF;
    std::map<nf_token, std::unique_ptr<Binding>> events, tasks, functions;
    std::map<std::string, int, std::less<>> modules;
    std::set<std::string, std::less<>> loading;
    Script(const nf_api *a, const nf_typed_api *t, nf_runtime *r, ScriptLimits l, std::string name,
           std::filesystem::path directory, std::string file, std::string &diagnostics)
        : api(a), typed(t), runtime(r), limits(l), id(std::move(name)), entry(std::move(file)),
          host_error(diagnostics), root(std::move(directory))
    {
    }
    ~Script()
    {
        if (context)
            api->context_close(runtime, context);
        if (state)
            lua_close(state);
        if (context)
            api->context_release(runtime, context);
    }
    static void *allocate(void *user, void *pointer, size_t old_size, size_t new_size)
    {
        auto &s = *static_cast<Script *>(user);
        if (!pointer)
            old_size = 0;
        if (!new_size)
        {
            std::free(pointer);
            s.metrics.memory_bytes -= old_size;
            return nullptr;
        }
        auto current = s.metrics.memory_bytes - old_size;
        if (new_size > s.limits.memory_bytes - current)
            return nullptr;
        void *result = std::realloc(pointer, new_size);
        if (result)
        {
            s.metrics.memory_bytes = current + new_size;
            s.metrics.peak_bytes = std::max(s.metrics.peak_bytes, s.metrics.memory_bytes);
        }
        return result;
    }
    static Script &self(lua_State *L)
    {
        return *(*static_cast<Frame **>(lua_getextraspace(L)))->script;
    }
    static void hook(lua_State *L, lua_Debug *)
    {
        auto &frame = **static_cast<Frame **>(lua_getextraspace(L));
        if (frame.remaining <= 100)
        {
            frame.exhausted = true;
            if (lua_isyieldable(L))
                lua_yield(L, 0);
            else
                luaL_error(L, "instruction limit exhausted");
        }
        else
            frame.remaining -= 100;
    }
    static void propagate_quota(lua_State *L)
    {
        if (!(*static_cast<Frame **>(lua_getextraspace(L)))->exhausted)
            return;
        if (lua_isyieldable(L))
            lua_yield(L, 0);
        else
            luaL_error(L, "instruction limit exhausted");
    }
    static int protected_finish(lua_State *L, int status, lua_KContext handler)
    {
        if (handler)
            lua_remove(L, 1);
        propagate_quota(L);
        lua_pushboolean(L, status == LUA_OK || status == LUA_YIELD);
        lua_insert(L, 1);
        return lua_gettop(L);
    }
    static int protected_call(lua_State *L)
    {
        propagate_quota(L);
        luaL_checkany(L, 1);
        int status = lua_pcallk(L, lua_gettop(L) - 1, LUA_MULTRET, 0, 0, protected_finish);
        return protected_finish(L, status, 0);
    }
    static int message_handler(lua_State *L)
    {
        auto *frame = *static_cast<Frame **>(lua_getextraspace(L));
        if (frame->exhausted)
        {
            lua_pushliteral(L, "instruction limit exhausted");
            return 1;
        }
        lua_pushvalue(L, lua_upvalueindex(1));
        lua_pushvalue(L, 1);
        int status = lua_pcall(L, 1, 1, 0);
        if (frame->exhausted)
            lua_pushliteral(L, "instruction limit exhausted");
        else if (status != LUA_OK)
            lua_pushliteral(L, "error in error handling");
        return 1;
    }
    static int protected_xcall(lua_State *L)
    {
        propagate_quota(L);
        luaL_checkany(L, 1);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        int arguments = lua_gettop(L) - 2;
        lua_pushvalue(L, 2);
        lua_pushcclosure(L, message_handler, 1);
        lua_remove(L, 2);
        lua_insert(L, 1);
        int status = lua_pcallk(L, arguments, LUA_MULTRET, 1, 1, protected_finish);
        return protected_finish(L, status, 1);
    }
    void fault(std::string_view message)
    {
        if (!metrics.faulted)
        {
            last_error = diagnostic(id, message);
            host_error = last_error;
            ++metrics.errors;
            metrics.faulted = true;
            if (context)
            {
                api->context_close(runtime, context);
                api->log(runtime, context, 3, span(last_error));
            }
        }
    }
    struct Operation
    {
        std::function<int(lua_State *)> fn;
    };
    static int trampoline(lua_State *L)
    {
        auto *operation = static_cast<Operation *>(lua_touserdata(L, 1));
        try
        {
            return operation->fn(L);
        }
        catch (const std::exception &e)
        {
            return luaL_error(L, "%s", e.what());
        }
    }
    bool protect(std::function<int(lua_State *)> fn)
    {
        Operation operation{std::move(fn)};
        lua_pushcfunction(state, trampoline);
        lua_pushlightuserdata(state, &operation);
        int status = lua_pcall(state, 1, 0, 0);
        if (status != LUA_OK)
        {
            fault(lua_error_text(state));
            lua_pop(state, 1);
            return false;
        }
        return !metrics.faulted;
    }
    bool invoke(int ref, std::function<int(lua_State *)> push = {},
                std::function<void(lua_State *)> consume = {})
    {
        if (metrics.faulted || depth >= 16)
        {
            if (!metrics.faulted)
                fault("script nesting exceeds 16");
            return false;
        }
        ++depth;
        ++metrics.calls;
        struct Depth
        {
            uint32_t &n;
            ~Depth() { --n; }
        } depth_guard{depth};
        return protect(
            [&](lua_State *L)
            {
                auto *thread = lua_newthread(L);
                Frame frame{this, limits.instructions};
                *static_cast<Frame **>(lua_getextraspace(thread)) = &frame;
                lua_sethook(thread, hook, LUA_MASKCOUNT, 100);
                lua_rawgeti(thread, LUA_REGISTRYINDEX, ref);
                int count = push ? push(thread) : 0;
                int results = 0;
                int status = lua_resume(thread, nullptr, count, &results);
                lua_sethook(thread, nullptr, 0, 0);
                if (status != LUA_OK || frame.exhausted)
                {
                    fault(frame.exhausted ? std::string_view("instruction limit exhausted")
                                          : lua_error_text(thread));
                }
                else if (consume)
                {
                    if (results)
                    {
                        lua_pushvalue(thread, -results);
                        lua_xmove(thread, L, 1);
                    }
                    else
                        lua_pushnil(L);
                    consume(L);
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
                return 0;
            });
    }
    static int settings_write(lua_State *L)
    {
        auto &s = self(L);
        size_t kn = 0, vn = 0;
        auto *key = luaL_checklstring(L, 1, &kn);
        auto *value = luaL_checklstring(L, 2, &vn);
        auto status = s.api->settings_write(s.runtime, s.context, {key, kn}, {value, vn});
        if (status != NF_OK)
            return luaL_error(L, "settings_write status %d", status);
        return 0;
    }
    static int settings_read(lua_State *L)
    {
        auto &s = self(L);
        size_t n = 0;
        const char *key = luaL_checklstring(L, 1, &n);
        uint64_t needed = 0;
        auto status = s.api->settings_read(s.runtime, s.context, {key, n}, nullptr, 0, &needed);
        if (status == NF_E_NOT_FOUND)
        {
            lua_pushnil(L);
            return 1;
        }
        if (status != NF_E_BUFFER)
            return luaL_error(L, "settings_read status %d", status);
        std::string buffer(static_cast<size_t>(needed), '\0');
        status =
            s.api->settings_read(s.runtime, s.context, {key, n}, buffer.data(), needed, &needed);
        if (status != NF_OK)
            return luaL_error(L, "settings_read status %d", status);
        lua_pushlstring(L, buffer.data(), buffer.size() - 1);
        return 1;
    }
    static int log(lua_State *L)
    {
        auto &s = self(L);
        size_t n = 0;
        const char *message = luaL_checklstring(L, 1, &n);
        auto status = s.api->log(s.runtime, s.context, 1, {message, n});
        if (status != NF_OK)
            return luaL_error(L, "log status %d", status);
        return 0;
    }
    static int require(lua_State *L)
    {
        auto &s = self(L);
        size_t n = 0;
        const char *raw = luaL_checklstring(L, 1, &n);
        std::string name(raw, n);
        if (!valid_id("module." + name))
            return luaL_error(L, "invalid local module name");
        auto found = s.modules.find(name);
        if (found != s.modules.end())
        {
            lua_rawgeti(L, LUA_REGISTRYINDEX, found->second);
            return 1;
        }
        if (s.loading.contains(name))
            return luaL_error(L, "cyclic local module");
        if (s.modules.size() + s.loading.size() >= s.limits.bindings)
            return luaL_error(L, "module limit");
        s.loading.insert(name);
        struct Loading
        {
            Script &s;
            std::string &name;
            ~Loading() { s.loading.erase(name); }
        } guard{s, name};
        std::string path = name;
        std::replace(path.begin(), path.end(), '.', '/');
        path += ".lua";
        std::string source;
        try
        {
            source = read_source(s.root, path);
        }
        catch (const std::exception &e)
        {
            return luaL_error(L, "%s", e.what());
        }
        if (luaL_loadbufferx(L, source.data(), source.size(), path.c_str(), "t") != LUA_OK)
            return lua_error(L);
        lua_callk(
            L, 0, 1, 0, +[](lua_State *thread, int, lua_KContext) -> int
            { return luaL_error(thread, "host does not resume yielded modules"); });
        if (lua_isnil(L, -1))
        {
            lua_pop(L, 1);
            lua_pushboolean(L, 1);
        }
        lua_pushvalue(L, -1);
        auto ref = luaL_ref(L, LUA_REGISTRYINDEX);
        try
        {
            s.modules.emplace(name, ref);
        }
        catch (...)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            throw;
        }
        return 1;
    }
    template <int (*Function)(lua_State *)> static int guarded(lua_State *L)
    {
        try
        {
            return Function(L);
        }
        catch (const std::exception &e)
        {
            return luaL_error(L, "%s", e.what());
        }
    }
    static uint32_t kind(lua_State *L, int index)
    {
        size_t size = 0;
        const char *text = luaL_checklstring(L, index, &size);
        std::string_view name(text, size);
        const char *names[] = {"void", "bool", "i64", "u64", "f64", "utf8", "bytes"};
        for (uint32_t i = 0; i < 7; ++i)
            if (name == names[i])
                return i;
        luaL_error(L, "unknown value kind");
        return 0;
    }
    static const char *kind_name(uint32_t kind)
    {
        const char *names[] = {"void", "bool", "i64", "u64", "f64", "utf8", "bytes"};
        return names[kind];
    }
    static int u64(lua_State *L)
    {
        if (lua_type(L, 1) != LUA_TSTRING)
            return luaL_error(L, "u64 needs an unsigned decimal string");
        size_t count = 0;
        const char *text = lua_tolstring(L, 1, &count);
        uint64_t value = 0;
        auto result = std::from_chars(text, text + count, value);
        if (result.ec != std::errc{} || result.ptr != text + count || !count)
            return luaL_error(L, "invalid uint64 decimal");
        auto *out = static_cast<uint64_t *>(lua_newuserdatauv(L, sizeof(value), 0));
        *out = value;
        luaL_setmetatable(L, "nf.uint64");
        return 1;
    }
    static int u64_text(lua_State *L)
    {
        auto value = *static_cast<uint64_t *>(luaL_checkudata(L, 1, "nf.uint64"));
        char text[24];
        auto result = std::to_chars(text, text + sizeof(text), value);
        lua_pushlstring(L, text, static_cast<size_t>(result.ptr - text));
        return 1;
    }
    static int u64_equal(lua_State *L)
    {
        auto *a = static_cast<uint64_t *>(luaL_testudata(L, 1, "nf.uint64"));
        auto *b = static_cast<uint64_t *>(luaL_testudata(L, 2, "nf.uint64"));
        lua_pushboolean(L, a && b && *a == *b);
        return 1;
    }
    static nf_value value_from(lua_State *L, int index, uint32_t type)
    {
        auto &s = self(L);
        nf_value value{};
        value.struct_size = sizeof(value);
        value.kind = type;
        switch (type)
        {
        case NF_VALUE_VOID:
            if (!lua_isnil(L, index))
                luaL_error(L, "void result must be nil");
            break;
        case NF_VALUE_BOOL:
            if (lua_type(L, index) != LUA_TBOOLEAN)
                luaL_error(L, "expected bool");
            value.as.boolean = lua_toboolean(L, index);
            break;
        case NF_VALUE_I64:
            if (!lua_isinteger(L, index))
                luaL_error(L, "expected exact i64");
            value.as.i64 = lua_tointeger(L, index);
            break;
        case NF_VALUE_U64:
            value.as.u64 = *static_cast<uint64_t *>(luaL_checkudata(L, index, "nf.uint64"));
            break;
        case NF_VALUE_F64:
            if (lua_type(L, index) != LUA_TNUMBER)
                luaL_error(L, "expected f64");
            if (lua_isinteger(L, index))
            {
                nf_value input{};
                input.struct_size = sizeof(input);
                input.kind = NF_VALUE_I64;
                input.as.i64 = lua_tointeger(L, index);
                if (s.typed->value_convert(&input, NF_VALUE_F64, &value) != NF_OK)
                    luaL_error(L, "integer loses f64 precision");
            }
            else
                value.as.f64 = lua_tonumber(L, index);
            break;
        case NF_VALUE_UTF8:
        case NF_VALUE_BYTES:
        {
            if (lua_type(L, index) != LUA_TSTRING)
                luaL_error(L, "expected string");
            size_t count = 0;
            const char *text = lua_tolstring(L, index, &count);
            if (type == NF_VALUE_UTF8)
                value.as.text = {text, count};
            else
                value.as.bytes = {text, count};
            break;
        }
        }
        nf_value checked{};
        checked.struct_size = sizeof(checked);
        if (s.typed->value_convert(&value, type, &checked) != NF_OK)
            luaL_error(L, "invalid typed value");
        return value;
    }
    static void push_value(lua_State *L, const nf_value &value)
    {
        switch (value.kind)
        {
        case NF_VALUE_VOID:
            lua_pushnil(L);
            break;
        case NF_VALUE_BOOL:
            lua_pushboolean(L, value.as.boolean);
            break;
        case NF_VALUE_I64:
            lua_pushinteger(L, value.as.i64);
            break;
        case NF_VALUE_U64:
            *static_cast<uint64_t *>(lua_newuserdatauv(L, sizeof(uint64_t), 0)) = value.as.u64;
            luaL_setmetatable(L, "nf.uint64");
            break;
        case NF_VALUE_F64:
            lua_pushnumber(L, value.as.f64);
            break;
        case NF_VALUE_UTF8:
            lua_pushlstring(L, value.as.text.data, static_cast<size_t>(value.as.text.size));
            break;
        case NF_VALUE_BYTES:
            lua_pushlstring(L, static_cast<const char *>(value.as.bytes.data),
                            static_cast<size_t>(value.as.bytes.size));
            break;
        }
    }
    static nf_function_info resolve(lua_State *L, std::vector<uint32_t> &arguments)
    {
        auto &s = self(L);
        size_t count = 0;
        const char *name = luaL_checklstring(L, 1, &count);
        nf_function_info info{};
        info.struct_size = sizeof(info);
        auto status =
            s.typed->function_resolve(s.runtime, s.context, {name, count}, &info, nullptr, 0);
        if (status != NF_OK && status != NF_E_BUFFER)
            luaL_error(L, "function_resolve status %d", status);
        arguments.resize(info.argument_count);
        status = s.typed->function_resolve(s.runtime, s.context, {name, count}, &info,
                                           arguments.data(), info.argument_count);
        if (status != NF_OK)
            luaL_error(L, "function_resolve status %d", status);
        return info;
    }
    static int signature(lua_State *L)
    {
        std::vector<uint32_t> arguments;
        auto info = resolve(L, arguments);
        lua_newtable(L);
        lua_pushstring(L, kind_name(info.result_kind));
        lua_setfield(L, -2, "result");
        lua_newtable(L);
        for (size_t i = 0; i < arguments.size(); ++i)
        {
            lua_pushstring(L, kind_name(arguments[i]));
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
        lua_setfield(L, -2, "arguments");
        return 1;
    }
    static int call(lua_State *L)
    {
        auto &s = self(L);
        std::vector<uint32_t> kinds;
        auto info = resolve(L, kinds);
        if (lua_gettop(L) != static_cast<int>(kinds.size() + 1))
            return luaL_error(L, "argument count mismatch");
        std::vector<nf_value> arguments;
        for (size_t i = 0; i < kinds.size(); ++i)
            arguments.push_back(value_from(L, static_cast<int>(i + 2), kinds[i]));
        nf_token token = 0;
        auto status =
            s.typed->function_invoke(s.runtime, s.context, info.function, arguments.data(),
                                     static_cast<uint32_t>(arguments.size()), &token);
        if (status != NF_OK)
            return luaL_error(L, "function_invoke status %d", status);
        struct Result
        {
            Script &s;
            nf_token token;
            ~Result() { s.typed->result_release(s.runtime, s.context, token); }
        } cleanup{s, token};
        nf_value result{};
        result.struct_size = sizeof(result);
        uint64_t needed = 0;
        status = s.typed->result_read(s.runtime, s.context, token, &result, nullptr, 0, &needed);
        std::string storage;
        if (status == NF_E_BUFFER)
        {
            storage.resize(static_cast<size_t>(needed));
            status = s.typed->result_read(s.runtime, s.context, token, &result, storage.data(),
                                          needed, &needed);
        }
        if (status != NF_OK)
            return luaL_error(L, "result_read status %d", status);
        push_value(L, result);
        return 1;
    }
    void check_binding_limit(lua_State *L)
    {
        if (events.size() + tasks.size() + functions.size() >= limits.bindings)
            luaL_error(L, "script binding limit");
    }
    struct Handle
    {
        Script *owner;
        nf_token token;
        uint32_t type;
    };
    static void push_handle(lua_State *L, Script &s, nf_token token, uint32_t type)
    {
        auto *out = static_cast<Handle *>(lua_newuserdatauv(L, sizeof(Handle), 0));
        *out = {&s, token, type};
        luaL_setmetatable(L, "nf.handle");
    }
    static nf_status NF_CALL native_callback(nf_context, nf_context, const nf_value *values,
                                             uint32_t count, nf_value *out, void *user)
    {
        auto &binding = *static_cast<Binding *>(user);
        auto &s = *binding.script;
        bool ok = s.invoke(
            binding.ref,
            [&](lua_State *L)
            {
                if (!lua_checkstack(L, static_cast<int>(count) + 8))
                    luaL_error(L, "callback stack exhausted");
                for (uint32_t i = 0; i < count; ++i)
                    push_value(L, values[i]);
                return static_cast<int>(count);
            },
            [&](lua_State *L)
            {
                auto result = value_from(L, -1, binding.result_kind);
                if (result.kind == NF_VALUE_UTF8)
                {
                    binding.returned.assign(result.as.text.data,
                                            static_cast<size_t>(result.as.text.size));
                    result.as.text = {binding.returned.data(), binding.returned.size()};
                }
                if (result.kind == NF_VALUE_BYTES)
                {
                    binding.returned.assign(static_cast<const char *>(result.as.bytes.data),
                                            static_cast<size_t>(result.as.bytes.size));
                    result.as.bytes = {binding.returned.data(), binding.returned.size()};
                }
                *out = result;
            });
        return ok ? NF_OK : NF_E_CALLBACK;
    }
    static int export_function(lua_State *L)
    {
        auto &s = self(L);
        s.check_binding_limit(L);
        size_t count = 0;
        const char *name = luaL_checklstring(L, 1, &count);
        auto binding = std::make_unique<Binding>();
        binding->script = &s;
        binding->result_kind = kind(L, 2);
        luaL_checktype(L, 3, LUA_TTABLE);
        luaL_checktype(L, 4, LUA_TFUNCTION);
        auto size = lua_rawlen(L, 3);
        if (size > 32)
            return luaL_error(L, "too many arguments");
        for (size_t i = 1; i <= size; ++i)
        {
            lua_rawgeti(L, 3, static_cast<lua_Integer>(i));
            auto type = kind(L, -1);
            lua_pop(L, 1);
            if (!type)
                return luaL_error(L, "void argument");
            binding->arguments.push_back(type);
        }
        lua_pushvalue(L, 4);
        binding->ref = luaL_ref(L, LUA_REGISTRYINDEX);
        nf_signature signature{sizeof(signature), binding->result_kind, static_cast<uint32_t>(size),
                               0, binding->arguments.data()};
        auto status = s.typed->function_register(s.runtime, s.context, {name, count}, &signature,
                                                 native_callback, binding.get(), &binding->token);
        if (status != NF_OK)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, binding->ref);
            return luaL_error(L, "function_register status %d", status);
        }
        auto token = binding->token;
        try
        {
            s.functions.emplace(token, std::move(binding));
        }
        catch (...)
        {
            s.typed->function_unregister(s.runtime, s.context, token);
            if (binding)
                luaL_unref(L, LUA_REGISTRYINDEX, binding->ref);
            throw;
        }
        return 0;
    }
    static void NF_CALL event_callback(nf_context, const nf_event *event, void *user)
    {
        auto &binding = *static_cast<Binding *>(user);
        auto *s = binding.script;
        int ref = binding.ref;
        bool ok = s->invoke(ref,
                            [&](lua_State *L)
                            {
                                lua_pushlstring(L, static_cast<const char *>(event->payload.data),
                                                static_cast<size_t>(event->payload.size));
                                nf_value sender{};
                                sender.kind = NF_VALUE_U64;
                                sender.as.u64 = event->sender;
                                push_value(L, sender);
                                return 2;
                            });
        if (!ok)
            throw std::runtime_error(s->last_error);
    }
    static void NF_CALL task_callback(nf_context, void *user)
    {
        auto *binding = static_cast<Binding *>(user);
        auto *s = binding->script;
        auto node = s->tasks.extract(binding->token);
        bool ok = s->invoke(binding->ref);
        luaL_unref(s->state, LUA_REGISTRYINDEX, binding->ref);
        if (!ok)
            throw std::runtime_error(s->last_error);
    }
    static int on(lua_State *L)
    {
        auto &s = self(L);
        s.check_binding_limit(L);
        size_t count = 0;
        const char *name = luaL_checklstring(L, 1, &count);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        push_handle(L, s, 0, 1);
        auto *handle = static_cast<Handle *>(lua_touserdata(L, -1));
        auto binding = std::make_unique<Binding>();
        binding->script = &s;
        lua_pushvalue(L, 2);
        binding->ref = luaL_ref(L, LUA_REGISTRYINDEX);
        auto status = s.api->event_subscribe(s.runtime, s.context, {name, count}, event_callback,
                                             binding.get(), &binding->token);
        if (status != NF_OK)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, binding->ref);
            return luaL_error(L, "event_subscribe status %d", status);
        }
        auto token = binding->token;
        try
        {
            s.events.emplace(token, std::move(binding));
        }
        catch (...)
        {
            s.api->event_unsubscribe(s.runtime, s.context, token);
            if (binding)
                luaL_unref(L, LUA_REGISTRYINDEX, binding->ref);
            throw;
        }
        handle->token = token;
        return 1;
    }
    static int defer(lua_State *L)
    {
        auto &s = self(L);
        s.check_binding_limit(L);
        luaL_checktype(L, 1, LUA_TFUNCTION);
        push_handle(L, s, 0, 2);
        auto *handle = static_cast<Handle *>(lua_touserdata(L, -1));
        auto binding = std::make_unique<Binding>();
        binding->script = &s;
        lua_pushvalue(L, 1);
        binding->ref = luaL_ref(L, LUA_REGISTRYINDEX);
        auto status = s.api->task_enqueue(s.runtime, s.context, task_callback, binding.get(),
                                          &binding->token);
        if (status != NF_OK)
        {
            luaL_unref(L, LUA_REGISTRYINDEX, binding->ref);
            return luaL_error(L, "task_enqueue status %d", status);
        }
        auto token = binding->token;
        try
        {
            s.tasks.emplace(token, std::move(binding));
        }
        catch (...)
        {
            s.api->task_cancel(s.runtime, s.context, token);
            if (binding)
                luaL_unref(L, LUA_REGISTRYINDEX, binding->ref);
            throw;
        }
        handle->token = token;
        return 1;
    }
    static int remove(lua_State *L, uint32_t type)
    {
        auto &s = self(L);
        auto *handle = static_cast<Handle *>(luaL_checkudata(L, 1, "nf.handle"));
        if (handle->owner != &s || handle->type != type)
            return luaL_error(L, "foreign handle kind/owner");
        auto &bindings = type == 1 ? s.events : s.tasks;
        auto found = bindings.find(handle->token);
        if (found == bindings.end())
        {
            lua_pushboolean(L, 0);
            return 1;
        }
        auto status = type == 1 ? s.api->event_unsubscribe(s.runtime, s.context, handle->token)
                                : s.api->task_cancel(s.runtime, s.context, handle->token);
        if (status != NF_OK)
            return luaL_error(L, "remove callback status %d", status);
        luaL_unref(L, LUA_REGISTRYINDEX, found->second->ref);
        bindings.erase(found);
        lua_pushboolean(L, 1);
        return 1;
    }
    static int off(lua_State *L) { return remove(L, 1); }
    static int cancel(lua_State *L) { return remove(L, 2); }
    static int emit(lua_State *L)
    {
        auto &s = self(L);
        size_t nn = 0, pn = 0;
        const char *name = luaL_checklstring(L, 1, &nn);
        const char *payload = luaL_checklstring(L, 2, &pn);
        auto status = s.api->event_emit(s.runtime, s.context, {name, nn}, {payload, pn});
        if (status != NF_OK)
            return luaL_error(L, "event_emit status %d", status);
        return 0;
    }
    void initialize(std::string_view source)
    {
        state = lua_newstate(allocate, this, 0);
        if (!state)
        {
            fault("cannot allocate Lua state");
            return;
        }
        *static_cast<Frame **>(lua_getextraspace(state)) = &main_frame;
        protect(
            [&](lua_State *L)
            {
                for (auto item : {std::pair<const char *, lua_CFunction>{LUA_GNAME, luaopen_base},
                                  {LUA_TABLIBNAME, luaopen_table},
                                  {LUA_STRLIBNAME, luaopen_string},
                                  {LUA_MATHLIBNAME, luaopen_math},
                                  {LUA_UTF8LIBNAME, luaopen_utf8}})
                {
                    luaL_requiref(L, item.first, item.second, 1);
                    lua_pop(L, 1);
                }
                for (auto name : {"dofile", "loadfile", "load", "collectgarbage", "print",
                                  "setmetatable", "getmetatable"})
                {
                    lua_pushnil(L);
                    lua_setglobal(L, name);
                }
                lua_pushcfunction(L, protected_call);
                lua_setglobal(L, "pcall");
                lua_pushcfunction(L, protected_xcall);
                lua_setglobal(L, "xpcall");
                lua_pushcfunction(L, guarded<require>);
                lua_setglobal(L, "require");
                luaL_newmetatable(L, "nf.uint64");
                lua_pushcfunction(L, u64_text);
                lua_setfield(L, -2, "__tostring");
                lua_pushcfunction(L, u64_equal);
                lua_setfield(L, -2, "__eq");
                lua_pushliteral(L, "uint64");
                lua_setfield(L, -2, "__metatable");
                lua_pop(L, 1);
                luaL_newmetatable(L, "nf.handle");
                lua_pushliteral(L, "callback handle");
                lua_setfield(L, -2, "__metatable");
                lua_pop(L, 1);
                lua_newtable(L);
                for (auto item : {std::pair<const char *, lua_CFunction>{"settings_write",
                                                                         guarded<settings_write>},
                                  {"settings_read", guarded<settings_read>},
                                  {"log", guarded<log>},
                                  {"u64", guarded<u64>},
                                  {"call", guarded<call>},
                                  {"signature", guarded<signature>},
                                  {"export", guarded<export_function>},
                                  {"on", guarded<on>},
                                  {"off", guarded<off>},
                                  {"defer", guarded<defer>},
                                  {"cancel", guarded<cancel>},
                                  {"emit", guarded<emit>}})
                {
                    lua_pushcfunction(L, item.second);
                    lua_setfield(L, -2, item.first);
                }
                lua_pushlstring(L, id.data(), id.size());
                lua_setfield(L, -2, "id");
                lua_setglobal(L, "nf");
                if (luaL_loadbufferx(L, source.data(), source.size(), (id + "/" + entry).c_str(),
                                     "t") != LUA_OK)
                    return lua_error(L);
                entry_ref = luaL_ref(L, LUA_REGISTRYINDEX);
                return 0;
            });
    }
};
} // namespace
struct ScriptHost::Impl
{
    const nf_api *api;
    const nf_typed_api *typed;
    nf_runtime *runtime;
    ScriptLimits limits;
    std::thread::id thread = std::this_thread::get_id();
    std::string last_error;
    std::map<std::string, std::unique_ptr<Script>, std::less<>> scripts;
    void check_thread() const
    {
        if (thread != std::this_thread::get_id())
            throw std::runtime_error("script host requires its creating thread");
    }
    Script &get(std::string_view id) const
    {
        check_thread();
        auto found = scripts.find(id);
        if (found == scripts.end())
            throw std::runtime_error("unknown script mod");
        return *found->second;
    }
    bool finish(Script &s, bool ok)
    {
        if (!ok)
            last_error = s.last_error;
        return ok;
    }
};
ScriptHost::ScriptHost(const nf_api *a, const nf_typed_api *t, nf_runtime *r, ScriptLimits limits)
    : impl(std::make_unique<Impl>())
{
    if (!a || a->struct_size != sizeof(*a) || a->abi_version != NF_ABI_VERSION || !t ||
        t->struct_size != sizeof(*t) || t->abi_version != NF_TYPED_ABI_VERSION || !r ||
        limits.memory_bytes < 65536 || limits.memory_bytes > 256 * 1048576 ||
        limits.instructions < 100 || limits.instructions > 100000000 || !limits.bindings ||
        limits.bindings > 4096)
        throw std::invalid_argument("invalid script host APIs/limits");
    impl->api = a;
    impl->typed = t;
    impl->runtime = r;
    impl->limits = limits;
}
ScriptHost::~ScriptHost() = default;
bool ScriptHost::prepare(std::string id, const std::filesystem::path &root, std::string entry)
{
    impl->check_thread();
    if (impl->scripts.contains(id))
    {
        impl->last_error = id + ": already loaded";
        return false;
    }
    try
    {
        auto stable_root = std::filesystem::absolute(root).lexically_normal();
        auto source = read_source(stable_root, entry);
        auto s = std::make_unique<Script>(impl->api, impl->typed, impl->runtime, impl->limits, id,
                                          stable_root, entry, impl->last_error);
        auto status = impl->api->context_create(impl->runtime, span(id), &s->context);
        if (status != NF_OK)
            throw std::runtime_error("cannot create script context");
        s->initialize(source);
        bool ok = impl->finish(*s, !s->metrics.faulted);
        impl->scripts.emplace(id, std::move(s));
        return ok;
    }
    catch (const std::exception &e)
    {
        impl->last_error = diagnostic(id, e.what());
        return false;
    }
}
bool ScriptHost::start(std::string_view id)
{
    auto &s = impl->get(id);
    return impl->finish(s, s.invoke(s.entry_ref));
}
bool ScriptHost::load(std::string id, const std::filesystem::path &root, std::string entry)
{
    return prepare(id, root, std::move(entry)) && start(id);
}
bool ScriptHost::execute(std::string_view id, std::string_view source)
{
    auto &s = impl->get(id);
    int ref = LUA_NOREF;
    if (s.metrics.faulted)
        return impl->finish(s, false);
    if (source.size() > 1048576)
    {
        s.fault("source exceeds 1 MiB");
        return impl->finish(s, false);
    }
    if (!s.protect(
            [&](lua_State *L)
            {
                if (luaL_loadbufferx(L, source.data(), source.size(), s.id.c_str(), "t") != LUA_OK)
                    return lua_error(L);
                ref = luaL_ref(L, LUA_REGISTRYINDEX);
                return 0;
            }))
        return impl->finish(s, false);
    bool ok = s.invoke(ref);
    luaL_unref(s.state, LUA_REGISTRYINDEX, ref);
    return impl->finish(s, ok);
}
bool ScriptHost::unload(std::string_view id)
{
    auto &s = impl->get(id);
    if (s.depth)
    {
        impl->last_error = s.id + ": script is executing";
        return false;
    }
    impl->scripts.erase(std::string(id));
    return true;
}
bool ScriptHost::reload(std::string_view id)
{
    auto &s = impl->get(id);
    auto name = s.id;
    auto root = s.root;
    auto entry = s.entry;
    if (!unload(id))
        return false;
    return load(std::move(name), root, std::move(entry));
}
nf_context ScriptHost::context(std::string_view id) const { return impl->get(id).context; }
ScriptStats ScriptHost::stats(std::string_view id) const
{
    auto &s = impl->get(id);
    auto stats = s.metrics;
    stats.bindings = static_cast<uint32_t>(s.events.size() + s.tasks.size() + s.functions.size());
    return stats;
}
std::string ScriptHost::error() const
{
    impl->check_thread();
    return impl->last_error;
}
bool ScriptHost::healthy() const
{
    impl->check_thread();
    for (auto &[id, s] : impl->scripts)
    {
        (void)id;
        if (s->metrics.faulted)
            return false;
    }
    return true;
}
} // namespace nf
