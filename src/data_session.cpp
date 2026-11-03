#include "data_internal.hpp"

namespace nf::data
{
using namespace detail;
ProjectSession::ProjectSession() : database_(std::make_unique<Database>(Json(Json::Object{}))) {}
void ProjectSession::check() const
{
    require(thread_ == std::this_thread::get_id(), "data session requires its creating thread");
}
bool ProjectSession::replace(std::string_view source, bool checkpoint)
{
    check();
    try
    {
        require(source.size() <= (checkpoint ? 32u : 1u) * 1048576,
                "data session input byte limit");
        auto parsed = checkpoint ? parse_json_exact_limited(source, 32 * 1048576, 262144)
                                 : parse_json_exact(source);
        std::unique_ptr<Database> candidate;
        if (checkpoint)
        {
            candidate = std::make_unique<Database>(parsed.at("schemas"));
            candidate->restore_checkpoint(parsed);
        }
        else
            candidate = build_project(parsed);
        // Ensure that every accepted session can be checkpointed before publishing.
        auto encoded = to_json(candidate->checkpoint());
        if (encoded != to_json(database_->checkpoint()))
        {
            require(generation_ != UINT64_MAX, "data session generation exhausted");
            database_ = std::move(candidate);
            ++generation_;
        }
        error_.clear();
        return true;
    }
    catch (const ParseError &e)
    {
        error_ = "data input at byte " + std::to_string(e.offset) + ": " + e.what();
    }
    catch (const std::bad_variant_access &)
    {
        error_ = "wrong JSON kind in data candidate";
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
bool ProjectSession::reload(std::string_view source) { return replace(source, false); }
bool ProjectSession::restore(std::string_view source) { return replace(source, true); }
Snapshot ProjectSession::snapshot() const
{
    check();
    return database_->snapshot();
}
Json ProjectSession::checkpoint() const
{
    check();
    return database_->checkpoint();
}
uint64_t ProjectSession::generation() const
{
    check();
    return generation_;
}
std::string_view ProjectSession::error() const
{
    check();
    return error_;
}
} // namespace nf::data
