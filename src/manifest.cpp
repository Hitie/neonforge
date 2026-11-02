#include "manifest.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
namespace nf
{
const std::map<std::string, uint32_t, std::less<>> &core_capabilities()
{
    static const std::map<std::string, uint32_t, std::less<>> c = {
        {"nf.core", 1},     {"nf.events", 1},   {"nf.tasks", 1},
        {"nf.services", 1}, {"nf.settings", 1}, {"nf.diagnostics", 1}};
    return c;
}
Version Version::parse(std::string_view text)
{
    Version out;
    uint32_t *parts[] = {&out.major, &out.minor, &out.patch};
    for (size_t i = 0; i < 3; ++i)
    {
        auto dot = text.find('.');
        auto part = text.substr(0, dot);
        if (part.empty() || (part.size() > 1 && part[0] == '0'))
            throw std::invalid_argument("version must be major.minor.patch without leading zeros");
        for (char c : part)
            if (c < '0' || c > '9')
                throw std::invalid_argument("invalid version digit");
        auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), *parts[i]);
        if (ec != std::errc() || end != part.data() + part.size())
            throw std::invalid_argument("version overflow");
        if (i < 2)
        {
            if (dot == text.npos)
                throw std::invalid_argument("incomplete version");
            text.remove_prefix(dot + 1);
        }
        else if (dot != text.npos)
            throw std::invalid_argument("too many version components");
    }
    return out;
}
std::string Version::str() const
{
    return std::to_string(major) + '.' + std::to_string(minor) + '.' + std::to_string(patch);
}
bool valid_id(std::string_view id)
{
    if (id.empty() || id.size() > 128 || id.find('.') == id.npos)
        return false;
    bool start = true;
    for (char c : id)
    {
        if (c == '.')
        {
            if (start)
                return false;
            start = true;
        }
        else
        {
            if (start && (c < 'a' || c > 'z'))
                return false;
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
                return false;
            start = false;
        }
    }
    return !start;
}
bool safe_relative_path(std::string_view path)
{
    if (path.empty() || path.size() > 240 || !valid_utf8(path))
        return false;
    size_t start = 0;
    for (size_t i = 0; i <= path.size(); ++i)
    {
        if (i < path.size() && path[i] != '/' && path[i] != '\\')
        {
            unsigned char c = static_cast<unsigned char>(path[i]);
            if (c < 32 ||
                std::string_view(":<>\"|?*").find(static_cast<char>(c)) != std::string_view::npos)
                return false;
            continue;
        }
        auto part = path.substr(start, i - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
            return false;
        auto base = std::string(part.substr(0, part.find('.')));
        for (char &c : base)
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 'a' + 'A');
        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
            (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) &&
             base[3] >= '1' && base[3] <= '9'))
            return false;
        start = i + 1;
    }
    return true;
}
namespace
{
void fields(const Json &j, std::initializer_list<std::string_view> allowed)
{
    for (auto &[key, v] : j.object())
    {
        (void)v;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            throw std::invalid_argument("unknown field: " + key);
    }
}
uint32_t integer(const Json &j)
{
    double n = j.number();
    if (n < 0 || n > std::numeric_limits<uint32_t>::max() || std::floor(n) != n)
        throw std::invalid_argument("expected unsigned integer");
    return static_cast<uint32_t>(n);
}
std::vector<std::string> ids(const Json *j)
{
    std::vector<std::string> out;
    if (j)
        for (auto &v : j->array())
        {
            auto id = v.string();
            if (!valid_id(id))
                throw std::invalid_argument("invalid mod ID: " + id);
            out.push_back(id);
        }
    return out;
}
} // namespace
Manifest parse_manifest(std::string_view text, std::filesystem::path directory)
{
    auto j = parse_json(text);
    fields(j, {"id", "name", "version", "api", "entry", "requires", "before", "after",
               "capabilities"});
    Manifest m;
    m.directory = std::move(directory);
    m.id = j.at("id").string();
    m.name = j.at("name").string();
    m.version = Version::parse(j.at("version").string());
    m.api = integer(j.at("api"));
    m.entry = j.at("entry").string();
    if (!valid_id(m.id))
        throw std::invalid_argument("invalid reverse-DNS mod ID");
    if (m.name.empty() || m.name.size() > 256 || m.name.find('\0') != m.name.npos)
        throw std::invalid_argument("invalid display name");
    if (m.api != 1)
        throw std::invalid_argument("unsupported API major");
    if (!safe_relative_path(m.entry) || std::filesystem::path(m.entry).extension() != L".dll")
        throw std::invalid_argument("entry must be an owned relative .dll path");
    m.before = ids(j.find("before"));
    m.after = ids(j.find("after"));
    if (auto deps = j.find("requires"))
    {
        std::set<std::string> seen;
        for (auto &d : deps->array())
        {
            fields(d, {"id", "min", "max_exclusive"});
            Dependency dep;
            dep.id = d.at("id").string();
            if (!valid_id(dep.id) || !seen.insert(dep.id).second)
                throw std::invalid_argument("invalid or duplicate dependency ID");
            dep.min = Version::parse(d.at("min").string());
            if (auto max = d.find("max_exclusive"))
            {
                dep.max_exclusive = Version::parse(max->string());
                if (*dep.max_exclusive <= dep.min)
                    throw std::invalid_argument("empty dependency version range");
            }
            m.requires_mods.push_back(std::move(dep));
        }
    }
    if (auto caps = j.find("capabilities"))
    {
        std::set<std::string> seen;
        for (auto &c : caps->array())
        {
            fields(c, {"name", "version", "required"});
            Requirement req;
            req.name = c.at("name").string();
            if (!valid_id(req.name) || !seen.insert(req.name).second)
                throw std::invalid_argument("invalid or duplicate capability");
            req.version = integer(c.at("version"));
            if (!req.version)
                throw std::invalid_argument("capability version must be positive");
            if (auto r = c.find("required"))
                req.required = r->boolean();
            m.capabilities.push_back(std::move(req));
        }
    }
    return m;
}
std::vector<size_t> resolve(const std::vector<Manifest> &mods,
                            const std::map<std::string, uint32_t, std::less<>> &available)
{
    if (mods.size() > 1024)
        throw std::invalid_argument("more than 1024 mods");
    std::map<std::string, size_t, std::less<>> byid;
    for (size_t i = 0; i < mods.size(); ++i)
    {
        auto &m = mods[i];
        if (!valid_id(m.id) || m.api != 1 || !byid.emplace(m.id, i).second)
            throw std::invalid_argument("invalid/duplicate mod ID or API: " + m.id);
        for (auto &c : m.capabilities)
        {
            auto a = available.find(c.name);
            if (c.required && (a == available.end() || a->second < c.version))
                throw std::invalid_argument(m.id + " requires unavailable capability " + c.name +
                                            " v" + std::to_string(c.version));
        }
    }
    std::vector<std::set<size_t>> edges(mods.size());
    std::vector<size_t> indegree(mods.size());
    auto edge = [&](size_t from, size_t to)
    {
        if (from == to)
            throw std::invalid_argument("self dependency/order: " + mods[to].id);
        if (edges[from].insert(to).second)
            ++indegree[to];
    };
    for (size_t i = 0; i < mods.size(); ++i)
    {
        auto &m = mods[i];
        for (auto &d : m.requires_mods)
        {
            auto dep = byid.find(d.id);
            if (dep == byid.end())
                throw std::invalid_argument(m.id + " missing dependency " + d.id);
            auto v = mods[dep->second].version;
            if (v < d.min || (d.max_exclusive && v >= *d.max_exclusive))
                throw std::invalid_argument(m.id + " incompatible dependency " + d.id +
                                            " (installed " + v.str() + ")");
            edge(dep->second, i);
        }
        for (auto &id : m.after)
            if (byid.contains(id))
                edge(byid.at(id), i);
        for (auto &id : m.before)
            if (byid.contains(id))
                edge(i, byid.at(id));
    }
    std::set<std::pair<std::string, size_t>> ready;
    for (size_t i = 0; i < mods.size(); ++i)
        if (!indegree[i])
            ready.emplace(mods[i].id, i);
    std::vector<size_t> out;
    while (!ready.empty())
    {
        auto i = ready.begin()->second;
        ready.erase(ready.begin());
        out.push_back(i);
        for (auto next : edges[i])
            if (!--indegree[next])
                ready.emplace(mods[next].id, next);
    }
    if (out.size() != mods.size())
    {
        std::string cycle = "dependency/order cycle among:";
        for (size_t i = 0; i < mods.size(); ++i)
            if (indegree[i])
                cycle += ' ' + mods[i].id;
        throw std::invalid_argument(cycle);
    }
    return out;
}
} // namespace nf
