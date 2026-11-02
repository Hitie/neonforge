#include "json.hpp"
#include <charconv>
#include <cmath>
#include <limits>
namespace nf
{
const Json::Object &Json::object() const { return std::get<Object>(value); }
const Json::Array &Json::array() const { return std::get<Array>(value); }
const std::string &Json::string() const { return std::get<std::string>(value); }
double Json::number() const { return std::get<double>(value); }
bool Json::boolean() const { return std::get<bool>(value); }
const Json &Json::at(std::string_view key) const { return object().at(std::string(key)); }
const Json *Json::find(std::string_view key) const
{
    auto i = object().find(key);
    return i == object().end() ? nullptr : &i->second;
}
bool valid_utf8(std::string_view s)
{
    for (size_t i = 0; i < s.size();)
    {
        auto lead = static_cast<unsigned char>(s[i++]);
        if (lead < 0x80)
            continue;
        uint32_t cp = 0, min = 0;
        size_t extra = 0;
        if (lead >= 0xc2 && lead <= 0xdf)
        {
            cp = lead & 31;
            extra = 1;
            min = 0x80;
        }
        else if (lead >= 0xe0 && lead <= 0xef)
        {
            cp = lead & 15;
            extra = 2;
            min = 0x800;
        }
        else if (lead >= 0xf0 && lead <= 0xf4)
        {
            cp = lead & 7;
            extra = 3;
            min = 0x10000;
        }
        else
            return false;
        if (extra > s.size() - i)
            return false;
        while (extra--)
        {
            auto c = static_cast<unsigned char>(s[i++]);
            if ((c & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (c & 63);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
    }
    return true;
}
namespace
{
struct Parser
{
    std::string_view s;
    size_t p = 0;
    [[noreturn]] void fail(std::string why) const { throw ParseError(p, std::move(why)); }
    void ws()
    {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n'))
            ++p;
    }
    bool take(char c)
    {
        if (p < s.size() && s[p] == c)
        {
            ++p;
            return true;
        }
        return false;
    }
    void need(char c)
    {
        if (!take(c))
            fail(std::string("expected '") + c + "'");
    }
    uint32_t hex4()
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (p == s.size())
                fail("incomplete Unicode escape");
            char c = s[p++];
            uint32_t n;
            if (c >= '0' && c <= '9')
                n = c - '0';
            else if (c >= 'a' && c <= 'f')
                n = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                n = c - 'A' + 10;
            else
                fail("invalid hex digit");
            v = (v << 4) | n;
        }
        return v;
    }
    static void utf8(std::string &out, uint32_t cp)
    {
        if (cp < 0x80)
            out += static_cast<char>(cp);
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 63));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 63));
            out += static_cast<char>(0x80 | (cp & 63));
        }
        else
        {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 63));
            out += static_cast<char>(0x80 | ((cp >> 6) & 63));
            out += static_cast<char>(0x80 | (cp & 63));
        }
    }
    std::string text()
    {
        need('"');
        std::string out;
        while (p < s.size())
        {
            unsigned char c = static_cast<unsigned char>(s[p++]);
            if (c == '"')
                return out;
            if (c < 32)
                fail("unescaped control character");
            if (c != '\\')
            {
                out += static_cast<char>(c);
                continue;
            }
            if (p == s.size())
                fail("incomplete escape");
            switch (s[p++])
            {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u':
            {
                uint32_t cp = hex4();
                if (cp >= 0xd800 && cp <= 0xdbff)
                {
                    need('\\');
                    need('u');
                    auto low = hex4();
                    if (low < 0xdc00 || low > 0xdfff)
                        fail("invalid surrogate pair");
                    cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                }
                else if (cp >= 0xdc00 && cp <= 0xdfff)
                    fail("unpaired surrogate");
                utf8(out, cp);
                break;
            }
            default:
                fail("unknown escape");
            }
        }
        fail("unterminated string");
    }
    Json value(unsigned depth)
    {
        if (depth > 64)
            fail("JSON nesting exceeds 64");
        ws();
        if (p == s.size())
            fail("expected value");
        if (s[p] == '"')
            return Json(text());
        if (take('{'))
        {
            Json::Object out;
            ws();
            if (take('}'))
                return Json(out);
            for (;;)
            {
                ws();
                auto key = text();
                ws();
                need(':');
                auto v = value(depth + 1);
                if (!out.emplace(std::move(key), std::move(v)).second)
                    fail("duplicate object key");
                ws();
                if (take('}'))
                    return Json(out);
                need(',');
            }
        }
        if (take('['))
        {
            Json::Array out;
            ws();
            if (take(']'))
                return Json(out);
            for (;;)
            {
                out.push_back(value(depth + 1));
                ws();
                if (take(']'))
                    return Json(out);
                need(',');
            }
        }
        for (auto [literal, v] : {std::pair<std::string_view, Json>{"true", Json(true)},
                                  {"false", Json(false)},
                                  {"null", Json()}})
        {
            if (s.substr(p, literal.size()) == literal)
            {
                p += literal.size();
                return v;
            }
        }
        auto start = p;
        take('-');
        if (!take('0'))
        {
            if (p == s.size() || s[p] < '1' || s[p] > '9')
                fail("expected number");
            while (p < s.size() && s[p] >= '0' && s[p] <= '9')
                ++p;
        }
        if (take('.'))
        {
            auto d = p;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9')
                ++p;
            if (p == d)
                fail("expected fractional digits");
        }
        if (take('e') || take('E'))
        {
            if (!take('+'))
                take('-');
            auto d = p;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9')
                ++p;
            if (p == d)
                fail("expected exponent digits");
        }
        double n = 0;
        auto [end, ec] = std::from_chars(s.data() + start, s.data() + p, n);
        if (ec != std::errc() || end != s.data() + p || !std::isfinite(n))
            fail("number outside finite range");
        return Json(n);
    }
};
void quoted(std::string &out, const std::string &text)
{
    if (!valid_utf8(text))
        throw std::invalid_argument("invalid UTF-8");
    out += '"';
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += static_cast<char>(c);
        }
        else if (c < 32)
        {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        }
        else
            out += static_cast<char>(c);
    }
    out += '"';
}
void serialize(std::string &out, const Json &j, unsigned depth)
{
    if (depth > 64)
        throw std::invalid_argument("JSON nesting exceeds 64");
    std::visit(
        [&](const auto &v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>)
                out += "null";
            else if constexpr (std::is_same_v<T, bool>)
                out += v ? "true" : "false";
            else if constexpr (std::is_same_v<T, double>)
            {
                if (!std::isfinite(v))
                    throw std::invalid_argument("nonfinite number");
                char buf[64];
                auto result = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general,
                                            std::numeric_limits<double>::max_digits10);
                if (result.ec != std::errc())
                    throw std::invalid_argument("number serialization failed");
                out.append(buf, result.ptr);
            }
            else if constexpr (std::is_same_v<T, std::string>)
                quoted(out, v);
            else if constexpr (std::is_same_v<T, Json::Array>)
            {
                out += '[';
                bool first = true;
                for (auto &item : v)
                {
                    if (!first)
                        out += ',';
                    first = false;
                    serialize(out, item, depth + 1);
                }
                out += ']';
            }
            else
            {
                out += '{';
                bool first = true;
                for (auto &[k, item] : v)
                {
                    if (!first)
                        out += ',';
                    first = false;
                    quoted(out, k);
                    out += ':';
                    serialize(out, item, depth + 1);
                }
                out += '}';
            }
        },
        j.value);
}
} // namespace
Json parse_json(std::string_view text)
{
    if (text.size() > 1048576)
        throw ParseError(0, "JSON exceeds 1 MiB");
    if (!valid_utf8(text))
        throw ParseError(0, "invalid UTF-8");
    Parser p{text};
    auto out = p.value(0);
    p.ws();
    if (p.p != text.size())
        p.fail("trailing data");
    return out;
}
std::string to_json(const Json &j)
{
    std::string out;
    serialize(out, j, 0);
    return out;
}
} // namespace nf
