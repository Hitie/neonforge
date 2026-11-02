#include <windows.h>

#include <filesystem>
#include <iostream>
#include <neonforge/api.h>

static bool entered = false;
static nf_status NF_CALL trap_service(nf_runtime *, nf_context, nf_str, uint32_t, uint32_t,
                                      nf_service *)
{
    entered = true;
    return NF_E_UNAVAILABLE;
}
static nf_status NF_CALL trap_log(nf_runtime *, nf_context, uint32_t, nf_str)
{
    entered = true;
    return NF_E_UNAVAILABLE;
}
int wmain(int argc, wchar_t **argv)
{
    if (argc < 2)
        return 2;
    int failures = 0;
    for (int i = 1; i < argc; ++i)
    {
        auto file = std::filesystem::absolute(argv[i]);
        HMODULE module = LoadLibraryExW(
            file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
        {
            ++failures;
            continue;
        }
        auto start =
            reinterpret_cast<nf_plugin_start_fn>(GetProcAddress(module, "nf_plugin_start"));
        if (!start)
        {
            FreeLibrary(module);
            ++failures;
            continue;
        }
        nf_api truncated{};
        truncated.struct_size = 8;
        truncated.abi_version = 1;
        truncated.service_acquire = trap_service;
        truncated.log = trap_log;
        nf_plugin_lifecycle lifecycle{};
        lifecycle.struct_size = sizeof(lifecycle);
        entered = false;
        auto status = start(&truncated, nullptr, 0, &lifecycle);
        if (status != NF_E_ABI || entered)
        {
            std::cerr << "FAIL plugin accepted a truncated ABI table\n";
            ++failures;
        }
        if (lifecycle.stop)
            lifecycle.stop(lifecycle.user);
        FreeLibrary(module);
    }
    if (!failures)
        std::cout << "PASS examples and generated plugin reject truncated API before callbacks\n";
    return failures ? 1 : 0;
}
