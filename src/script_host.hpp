#pragma once
#include <filesystem>
#include <memory>
#include <neonforge/typed.h>
#include <string>
#include <string_view>

namespace nf
{
struct ScriptLimits
{
    uint64_t memory_bytes = 8 * 1048576;
    uint64_t instructions = 1000000;
    uint32_t bindings = 512;
};
struct ScriptStats
{
    uint64_t memory_bytes = 0, peak_bytes = 0, calls = 0, errors = 0;
    uint32_t bindings = 0;
    bool faulted = false;
};
class ScriptHost
{
    struct Impl;
    std::unique_ptr<Impl> impl;

  public:
    ScriptHost(const nf_api *, const nf_typed_api *, nf_runtime *, ScriptLimits = {});
    ~ScriptHost();
    ScriptHost(const ScriptHost &) = delete;
    ScriptHost &operator=(const ScriptHost &) = delete;
    bool prepare(std::string id, const std::filesystem::path &root, std::string entry);
    bool start(std::string_view id);
    bool load(std::string id, const std::filesystem::path &root, std::string entry);
    bool execute(std::string_view id, std::string_view source);
    bool reload(std::string_view id);
    bool unload(std::string_view id);
    nf_context context(std::string_view id) const;
    ScriptStats stats(std::string_view id) const;
    std::string error() const;
    bool healthy() const;
};
} // namespace nf
