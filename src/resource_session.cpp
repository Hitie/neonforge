#include "data_internal.hpp"
#include "resource_model.hpp"

namespace nf::resources
{
void ResourceSession::check() const
{
    nf::data::detail::require(thread_ == std::this_thread::get_id(),
                              "resource session requires its creating thread");
}
bool ResourceSession::reload(std::string_view source)
{
    check();
    try
    {
        auto candidate = std::make_shared<const Json>(compose_resources(parse_json_exact(source)));
        if (!plan_ || plan_->at("cache_key").string() != candidate->at("cache_key").string())
        {
            nf::data::detail::require(generation_ != UINT64_MAX, "resource generation exhausted");
            plan_ = std::move(candidate);
            ++generation_;
        }
        error_.clear();
        return true;
    }
    catch (const ParseError &e)
    {
        error_ = "resource input at byte " + std::to_string(e.offset) + ": " + e.what();
    }
    catch (const std::bad_variant_access &)
    {
        error_ = "wrong JSON kind in resource project";
    }
    catch (const std::exception &e)
    {
        error_ = e.what();
    }
    if (error_.size() > 4096)
        error_.resize(4096);
    while (!valid_utf8(error_))
        error_.pop_back();
    return false;
}
std::shared_ptr<const Json> ResourceSession::snapshot() const
{
    check();
    nf::data::detail::require(bool(plan_), "resource session has no valid plan");
    return plan_;
}
uint64_t ResourceSession::generation() const
{
    check();
    return generation_;
}
std::string_view ResourceSession::error() const
{
    check();
    return error_;
}
} // namespace nf::resources
