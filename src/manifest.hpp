#pragma once
#include "json.hpp"
#include <filesystem>
#include <optional>
#include <set>
namespace nf
{
struct Version
{
    uint32_t major = 0, minor = 0, patch = 0;
    auto operator<=>(const Version &) const = default;
    static Version parse(std::string_view text);
    std::string str() const;
};
struct Dependency
{
    std::string id;
    Version min;
    std::optional<Version> max_exclusive;
};
struct Requirement
{
    std::string name;
    uint32_t version = 1;
    bool required = true;
};
struct Manifest
{
    std::string id, name, entry;
    Version version;
    uint32_t api = 1;
    std::vector<Dependency> requires_mods;
    std::vector<std::string> before, after;
    std::vector<Requirement> capabilities;
    std::filesystem::path directory;
};
bool valid_id(std::string_view id);
bool safe_relative_path(std::string_view path);
const std::map<std::string, uint32_t, std::less<>> &core_capabilities();
Manifest parse_manifest(std::string_view json, std::filesystem::path directory = {});
std::vector<size_t> resolve(const std::vector<Manifest> &mods,
                            const std::map<std::string, uint32_t, std::less<>> &available);
} // namespace nf
