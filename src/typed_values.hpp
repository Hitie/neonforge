#pragma once
#include "json.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <neonforge/typed.h>
#include <string>

namespace nf
{
inline constexpr uint64_t value_payload_limit = 1048576;
inline bool valid_value(const nf_value &v)
{
    if (v.struct_size != sizeof(v) || v.kind > NF_VALUE_BYTES)
        return false;
    switch (v.kind)
    {
    case NF_VALUE_BOOL:
        return v.as.boolean <= 1;
    case NF_VALUE_F64:
        return std::isfinite(v.as.f64);
    case NF_VALUE_UTF8:
        return v.as.text.size <= value_payload_limit && (v.as.text.data || !v.as.text.size) &&
               valid_utf8(
                   {v.as.text.data ? v.as.text.data : "", static_cast<size_t>(v.as.text.size)});
    case NF_VALUE_BYTES:
        return v.as.bytes.size <= value_payload_limit && (v.as.bytes.data || !v.as.bytes.size);
    default:
        return true;
    }
}
inline bool exact_double_integer(uint64_t magnitude)
{
    auto bits = std::bit_width(magnitude);
    return bits <= 53 || !(magnitude & ((uint64_t(1) << (bits - 53)) - 1));
}
inline nf_status NF_CALL convert_value(const nf_value *input, uint32_t target, nf_value *output)
{
    if (!input || !output || output->struct_size != sizeof(*output) || !valid_value(*input) ||
        target > NF_VALUE_BYTES)
        return NF_E_ARGUMENT;
    auto converted = *input;
    if (input->kind == target)
    {
        *output = converted;
        return NF_OK;
    }
    converted.kind = target;
    if (input->kind == NF_VALUE_I64)
    {
        auto n = input->as.i64;
        if (target == NF_VALUE_U64 && n >= 0)
            converted.as.u64 = static_cast<uint64_t>(n);
        else if (target == NF_VALUE_F64 &&
                 exact_double_integer(n < 0 ? uint64_t(-(n + 1)) + 1 : uint64_t(n)))
            converted.as.f64 = static_cast<double>(n);
        else
            return NF_E_ARGUMENT;
    }
    else if (input->kind == NF_VALUE_U64)
    {
        auto n = input->as.u64;
        if (target == NF_VALUE_I64 && n <= uint64_t(INT64_MAX))
            converted.as.i64 = static_cast<int64_t>(n);
        else if (target == NF_VALUE_F64 && exact_double_integer(n))
            converted.as.f64 = static_cast<double>(n);
        else
            return NF_E_ARGUMENT;
    }
    else if (input->kind == NF_VALUE_F64)
    {
        auto n = input->as.f64;
        if (std::trunc(n) != n)
            return NF_E_ARGUMENT;
        if (target == NF_VALUE_I64 && n >= -0x1p63 && n < 0x1p63)
            converted.as.i64 = static_cast<int64_t>(n);
        else if (target == NF_VALUE_U64 && n >= 0 && n < 0x1p64)
            converted.as.u64 = static_cast<uint64_t>(n);
        else
            return NF_E_ARGUMENT;
    }
    else
        return NF_E_ARGUMENT;
    *output = converted;
    return NF_OK;
}
struct OwnedValue
{
    nf_value value{};
    std::string storage;
    explicit OwnedValue(const nf_value &source) : value(source)
    {
        if (source.kind == NF_VALUE_UTF8)
        {
            storage.assign(source.as.text.data ? source.as.text.data : "",
                           static_cast<size_t>(source.as.text.size));
            value.as.text = {};
        }
        else if (source.kind == NF_VALUE_BYTES)
        {
            storage.assign(source.as.bytes.data ? static_cast<const char *>(source.as.bytes.data)
                                                : "",
                           static_cast<size_t>(source.as.bytes.size));
            value.as.bytes = {};
        }
    }
};
} // namespace nf
