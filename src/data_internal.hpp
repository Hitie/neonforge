#pragma once
#include "data_model.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace nf::data::detail
{
bool exact_small_integer(const Json &, uint32_t);
inline void require(bool ok, std::string message)
{
    if (!ok)
        throw std::invalid_argument(std::move(message));
}
inline void fields(const Json &json, std::initializer_list<std::string_view> allowed)
{
    for (const auto &[name, value] : json.object())
    {
        (void)value;
        require(std::find(allowed.begin(), allowed.end(), name) != allowed.end(),
                "unknown data field: " + name);
    }
}
inline void identifier(std::string_view name, size_t limit = 128)
{
    require(!name.empty() && name.size() <= limit, "invalid data identifier length");
    for (unsigned char c : name)
        require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '_' || c == '.' || c == '-',
                "invalid data identifier");
}
inline void text(std::string_view value, size_t limit, bool nul = false)
{
    require(value.size() <= limit && valid_utf8(value) && (nul || value.find('\0') == value.npos),
            "invalid data text");
}
inline Json number(uint64_t value) { return Json(std::to_string(value)); }
inline Json string(std::string value) { return Json(std::move(value)); }
} // namespace nf::data::detail
