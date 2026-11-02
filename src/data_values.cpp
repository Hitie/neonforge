#include "data_internal.hpp"
#include "manifest.hpp"
#include <charconv>
#include <functional>

namespace nf::data
{
using namespace detail;
bool detail::exact_small_integer(const Json &json, uint32_t expected)
{
    if (json.number() != expected)
        return false;
    auto token = json.number_token();
    if (token.empty())
        return true;
    bool negative = token.front() == '-';
    if (negative)
        token.remove_prefix(1);
    auto exponent_pos = token.find_first_of("eE");
    auto mantissa = token.substr(0, exponent_pos);
    std::string digits;
    int64_t fraction = 0;
    bool dot = false;
    for (char c : mantissa)
    {
        if (c == '.')
        {
            dot = true;
            continue;
        }
        digits += c;
        if (dot)
            ++fraction;
    }
    auto start = digits.find_first_not_of('0');
    if (start == digits.npos)
        return expected == 0;
    if (negative || !expected)
        return false;
    digits.erase(0, start);
    int64_t exponent = 0;
    if (exponent_pos != token.npos)
    {
        auto exp = token.substr(exponent_pos + 1);
        bool minus = exp.front() == '-';
        if (exp.front() == '+' || minus)
            exp.remove_prefix(1);
        for (char c : exp)
            exponent = std::min<int64_t>(2000000, exponent * 10 + (c - '0'));
        if (minus)
            exponent = -exponent;
    }
    int64_t scale = exponent - fraction;
    while (digits.back() == '0')
    {
        digits.pop_back();
        ++scale;
    }
    auto target = std::to_string(expected);
    if (scale < 0 || scale > 3 || digits.size() + static_cast<size_t>(scale) != target.size())
        return false;
    return target == digits + std::string(static_cast<size_t>(scale), '0');
}
namespace
{
void strip_number_tokens(Json &json)
{
    if (auto number = std::get_if<Json::Number>(&json.value))
    {
        double value = number->value;
        json.value = value;
        return;
    }
    if (auto array = std::get_if<Json::Array>(&json.value))
        for (auto &item : *array)
            strip_number_tokens(item);
    if (auto object = std::get_if<Json::Object>(&json.value))
        for (auto &[key, item] : *object)
        {
            (void)key;
            strip_number_tokens(item);
        }
}
void bounded_json(const Json &json, size_t &nodes, size_t &bytes, unsigned depth = 0)
{
    require(++nodes <= 65536 && depth <= 48, "data value structure limit");
    std::visit(
        [&](const auto &value)
        {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::string>)
            {
                bytes += value.size();
                require(bytes <= 1048576, "data value text limit");
                text(value, 1048576, true);
            }
            else if constexpr (std::is_same_v<T, Json::Array>)
                for (const auto &item : value)
                    bounded_json(item, nodes, bytes, depth + 1);
            else if constexpr (std::is_same_v<T, Json::Object>)
                for (const auto &[key, item] : value)
                {
                    bytes += key.size();
                    require(bytes <= 1048576, "data value text limit");
                    bounded_json(item, nodes, bytes, depth + 1);
                }
        },
        json.value);
}
void check_type(const Json &json, unsigned depth)
{
    require(depth <= 16, "data array type nesting exceeds 16");
    const auto &kind = json.at("type").string();
    if (kind == "array")
    {
        fields(json, {"type", "element"});
        check_type(json.at("element"), depth + 1);
    }
    else if (kind == "enum")
    {
        fields(json, {"type", "enum_type"});
        identifier(json.at("enum_type").string());
    }
    else
    {
        fields(json, {"type"});
        constexpr std::string_view kinds[] = {
            "bool",     "i64",          "u64",     "f64",     "string",  "name",       "id",
            "resource", "localization", "vector2", "vector3", "vector4", "quaternion", "color"};
        require(std::find(std::begin(kinds), std::end(kinds), kind) != std::end(kinds),
                "unknown data type: " + kind);
    }
}
template <class T> void integer(const std::string &input)
{
    T value{};
    auto result = std::from_chars(input.data(), input.data() + input.size(), value);
    require(result.ec == std::errc{} && result.ptr == input.data() + input.size() &&
                std::to_string(value) == input,
            "noncanonical or out-of-range data integer");
}
} // namespace
Type::Type(Json descriptor) : descriptor_(std::move(descriptor)), key_(to_json(descriptor_)) {}
Type Type::parse(const Json &json)
{
    size_t nodes = 0, bytes = 0;
    bounded_json(json, nodes, bytes);
    check_type(json, 0);
    return Type(json);
}
Type Type::element() const
{
    require(kind() == "array", "type is not an array");
    return parse(descriptor_.at("element"));
}
struct Value::Rep
{
    Type type;
    Json json;
    std::string encoded;
};
Value::Value(std::shared_ptr<const Rep> rep) : rep_(std::move(rep)) {}
Value Value::parse(const Json &json)
{
    size_t nodes = 0, bytes = 0;
    bounded_json(json, nodes, bytes);
    auto descriptor = json.object();
    require(descriptor.erase("value") == 1, "missing data value");
    auto type = Type::parse(Json(descriptor));
    const auto &value = json.at("value");
    auto kind = type.kind();
    if (kind == "bool")
        (void)value.boolean();
    else if (kind == "i64")
        integer<int64_t>(value.string());
    else if (kind == "u64" || kind == "id")
        integer<uint64_t>(value.string());
    else if (kind == "f64")
        require(std::isfinite(value.number()), "nonfinite data number");
    else if (kind == "array")
    {
        require(value.array().size() <= 4096, "data array length exceeds 4096");
        auto element = type.element();
        for (const auto &item : value.array())
            require(Value::parse(item).type() == element, "data array element type mismatch");
    }
    else if (kind == "vector2" || kind == "vector3" || kind == "vector4" || kind == "quaternion" ||
             kind == "color")
    {
        size_t count = kind == "vector2" ? 2 : kind == "vector3" ? 3 : 4;
        require(value.array().size() == count, "invalid data component count");
        for (const auto &component : value.array())
        {
            double n = component.number();
            require(std::isfinite(n), "nonfinite data component");
            if (kind == "color")
                require(n >= 0 && n <= 255 && std::trunc(n) == n &&
                            exact_small_integer(component, static_cast<uint32_t>(n)),
                        "color channels must be integers 0..255");
        }
    }
    else
    {
        text(value.string(),
             kind == "string"     ? 1048576
             : kind == "resource" ? 240
                                  : 128,
             kind == "string");
        if (kind == "enum")
            identifier(value.string());
        if (kind == "resource")
            require(safe_relative_path(value.string()), "invalid data resource path");
        if (kind != "string")
            require(!value.string().empty(), "empty symbolic data value");
    }
    auto encoded = to_json(json);
    require(encoded.size() <= 1048576, "encoded data value exceeds 1 MiB");
    auto owned = json;
    strip_number_tokens(owned);
    return Value(
        std::make_shared<const Rep>(Rep{std::move(type), std::move(owned), std::move(encoded)}));
}
const Value::Rep &Value::live() const
{
    require(bool(rep_), "data value was moved from");
    return *rep_;
}
const Type &Value::type() const { return live().type; }
const Json &Value::json() const { return live().json; }
const std::string &Value::encoded() const { return live().encoded; }
bool Value::operator==(const Value &other) const
{
    (void)live();
    (void)other.live();
    return rep_ == other.rep_ || encoded() == other.encoded();
}
} // namespace nf::data
