// Deliberately defective test adapter: cancellation claims success without removing work.
#define NOMINMAX
#define NF_BUILD_CORE
#include <filesystem>
#include <neonforge/api.h>
#include <windows.h>

namespace
{
nf_status NF_CALL no_cancel(nf_runtime *, nf_context, nf_token) { return NF_OK; }
} // namespace
extern "C" NF_EXPORT nf_status NF_CALL nf_get_api(uint32_t abi, uint32_t size, const nf_api **out)
{
    if (!out)
        return NF_E_ARGUMENT;
    *out = nullptr;
    if (abi != NF_ABI_VERSION || size != sizeof(nf_api))
        return NF_E_ABI;
    static HMODULE real = []
    {
        wchar_t path[32768];
        auto length = GetModuleFileNameW(nullptr, path, 32768);
        if (!length || length >= 32768)
            return HMODULE(nullptr);
        auto dll = std::filesystem::path(path).parent_path() / L"NeonForge.Core.dll";
        return LoadLibraryExW(dll.c_str(), nullptr,
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    }();
    if (!real)
        return NF_E_IO;
    using GetApi = nf_status(NF_CALL *)(uint32_t, uint32_t, const nf_api **);
    auto get = reinterpret_cast<GetApi>(GetProcAddress(real, "nf_get_api"));
    if (!get)
        return NF_E_ABI;
    const nf_api *source = nullptr;
    auto status = get(abi, size, &source);
    if (status != NF_OK || !source || source->struct_size != sizeof(nf_api))
        return NF_E_ABI;
    static nf_api table = *source;
    table.task_cancel = no_cancel;
    *out = &table;
    return NF_OK;
}
