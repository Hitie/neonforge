#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include "resource_model.hpp"
#include <Windows.h>
#include <array>
#include <bcrypt.h>
#include <limits>

namespace nf::resources
{
std::string resource_digest(std::string_view bytes)
{
    if (bytes.size() > std::numeric_limits<ULONG>::max())
        throw std::invalid_argument("resource digest input limit");
    std::array<unsigned char, 32> digest{};
    auto status = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                             reinterpret_cast<unsigned char *>(const_cast<char *>(bytes.data())),
                             static_cast<ULONG>(bytes.size()), digest.data(),
                             static_cast<ULONG>(digest.size()));
    if (status < 0)
        throw std::runtime_error("Windows SHA-256 failed");
    std::string result;
    constexpr char hex[] = "0123456789abcdef";
    for (auto value : digest)
    {
        result += hex[value >> 4];
        result += hex[value & 15];
    }
    return result;
}
} // namespace nf::resources
