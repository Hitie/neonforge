#pragma once
#include <map>
#include <memory>
#include <neonforge/api.h>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace nf
{
// Only subscription identities are cached. Ownership and callback liveness stay canonical.
class EventRoutes
{
    struct Channel
    {
        std::set<nf_token> live;
        std::shared_ptr<const std::vector<nf_token>> cached;
    };
    std::map<std::string, Channel, std::less<>> channels;

  public:
    void add(const std::string &name, nf_token token)
    {
        auto [entry, created] = channels.try_emplace(name);
        try
        {
            if (entry->second.live.insert(token).second)
                entry->second.cached.reset();
        }
        catch (...)
        {
            if (created)
                channels.erase(entry);
            throw;
        }
    }
    void remove(std::string_view name, nf_token token)
    {
        auto entry = channels.find(name);
        if (entry == channels.end() || !entry->second.live.erase(token))
            return;
        if (entry->second.live.empty())
            channels.erase(entry);
        else
            entry->second.cached.reset();
    }
    std::shared_ptr<const std::vector<nf_token>> snapshot(std::string_view name)
    {
        auto entry = channels.find(name);
        if (entry == channels.end())
            return {};
        auto &channel = entry->second;
        if (!channel.cached)
            channel.cached = std::make_shared<const std::vector<nf_token>>(channel.live.begin(),
                                                                           channel.live.end());
        return channel.cached;
    }
};
} // namespace nf
