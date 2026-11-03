#pragma once
#include "json.hpp"
#include <cstdint>
#include <memory>
#include <thread>

namespace nf::resources
{
Json compose_resources(const Json &);
std::string resource_digest(std::string_view);
class ResourceSession
{
    std::thread::id thread_ = std::this_thread::get_id();
    std::shared_ptr<const Json> plan_;
    uint64_t generation_ = 0;
    std::string error_;
    void check() const;

  public:
    ResourceSession() = default;
    ResourceSession(const ResourceSession &) = delete;
    ResourceSession &operator=(const ResourceSession &) = delete;
    bool reload(std::string_view);
    std::shared_ptr<const Json> snapshot() const;
    uint64_t generation() const;
    std::string_view error() const;
};
} // namespace nf::resources
