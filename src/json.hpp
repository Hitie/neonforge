#pragma once
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
namespace nf
{
struct Json
{
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value = nullptr;
    Json() = default;
    template <class T> explicit Json(T v) : value(std::move(v)) {}
    const Object &object() const;
    const Array &array() const;
    const std::string &string() const;
    double number() const;
    bool boolean() const;
    const Json &at(std::string_view key) const;
    const Json *find(std::string_view key) const;
};
struct ParseError : std::runtime_error
{
    size_t offset;
    ParseError(size_t p, std::string message) : runtime_error(std::move(message)), offset(p) {}
};
bool valid_utf8(std::string_view text);
Json parse_json(std::string_view text);
std::string to_json(const Json &value);
} // namespace nf
