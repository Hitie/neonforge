#define NOMINMAX
#include <windows.h>

#include "json.hpp"
#include <iostream>
#include <limits>
#include <mmsystem.h>

// This executable probes the Windows library. It is NOT a game-loadable proxy.
int main()
{
    HMODULE library = nullptr;
    try
    {
        wchar_t system[MAX_PATH];
        UINT length = GetSystemDirectoryW(system, MAX_PATH);
        if (!length || length >= MAX_PATH)
            throw std::runtime_error("Cannot resolve Windows system directory");
        std::wstring path(system, length);
        path += L"\\winmm.dll";
        library = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library)
            throw std::runtime_error("Cannot load Windows system WINMM");
        auto base = reinterpret_cast<const unsigned char *>(library);
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
            throw std::runtime_error("Invalid loaded DOS header");
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            throw std::runtime_error("Unexpected system image architecture");
        DWORD image_size = nt->OptionalHeader.SizeOfImage;
        auto range = [&](DWORD offset, size_t bytes)
        {
            if (offset >= image_size || bytes > image_size - offset)
                throw std::runtime_error("Export metadata outside loaded image");
        };
        auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        range(directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY));
        auto exports =
            reinterpret_cast<const IMAGE_EXPORT_DIRECTORY *>(base + directory.VirtualAddress);
        range(exports->AddressOfNames, static_cast<size_t>(exports->NumberOfNames) * sizeof(DWORD));
        range(exports->AddressOfFunctions,
              static_cast<size_t>(exports->NumberOfFunctions) * sizeof(DWORD));
        auto names = reinterpret_cast<const DWORD *>(base + exports->AddressOfNames);
        auto functions = reinterpret_cast<const DWORD *>(base + exports->AddressOfFunctions);
        DWORD resolved_names = 0, resolved_ordinals = 0, missing = 0;
        for (DWORD i = 0; i < exports->NumberOfNames; ++i)
        {
            range(names[i], 1);
            auto name = reinterpret_cast<const char *>(base + names[i]);
            bool terminated = false;
            for (size_t n = names[i]; n < image_size; ++n)
                if (!base[n])
                {
                    terminated = true;
                    break;
                }
            if (!terminated)
                throw std::runtime_error("Unterminated exported name");
            if (GetProcAddress(library, name))
                ++resolved_names;
            else
                ++missing;
        }
        for (DWORD i = 0; i < exports->NumberOfFunctions; ++i)
        {
            if (!functions[i])
                continue;
            uint64_t ordinal = static_cast<uint64_t>(exports->Base) + i;
            if (ordinal > std::numeric_limits<WORD>::max())
                throw std::runtime_error("Ordinal outside Windows export range");
            if (GetProcAddress(library, MAKEINTRESOURCEA(static_cast<WORD>(ordinal))))
                ++resolved_ordinals;
            else
                ++missing;
        }
        using Timer = DWORD(WINAPI *)();
        using Caps = MMRESULT(WINAPI *)(LPTIMECAPS, UINT);
        auto timer = reinterpret_cast<Timer>(GetProcAddress(library, "timeGetTime"));
        auto caps = reinterpret_cast<Caps>(GetProcAddress(library, "timeGetDevCaps"));
        if (!timer || !caps)
            throw std::runtime_error("Required timer exports missing");
        TIMECAPS capability{};
        bool caps_ok = caps(&capability, sizeof(capability)) == TIMERR_NOERROR;
        DWORD before = timer();
        Sleep(2);
        DWORD delta = timer() - before; // DWORD wraparound is intentional.
        nf::Json::Object result;
        result.emplace("named_exports", nf::Json(static_cast<double>(exports->NumberOfNames)));
        result.emplace("resolved_names", nf::Json(static_cast<double>(resolved_names)));
        result.emplace("resolved_ordinals", nf::Json(static_cast<double>(resolved_ordinals)));
        result.emplace("unresolved_exports", nf::Json(static_cast<double>(missing)));
        result.emplace("timer_caps_ok", nf::Json(caps_ok));
        result.emplace("timer_delta_ms", nf::Json(static_cast<double>(delta)));
        result.emplace("game_bootstrap_verified", nf::Json(false));
        result.emplace("engine_adapter_ready", nf::Json(false));
        std::cout << nf::to_json(nf::Json(result)) << '\n';
        FreeLibrary(library);
        return missing || !caps_ok ? 1 : 0;
    }
    catch (const std::exception &error)
    {
        if (library)
            FreeLibrary(library);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
