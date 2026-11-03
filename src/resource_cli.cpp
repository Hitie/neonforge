#include "resource_model.hpp"
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <iostream>

namespace
{
std::string diagnostic(std::string value)
{
    if (value.size() > 4096)
        value.resize(4096);
    while (!nf::valid_utf8(value))
        value.pop_back();
    return value;
}
} // namespace
int wmain(int argc, wchar_t **argv)
{
    try
    {
        if (argc != 3 ||
            (std::wstring_view(argv[1]) != L"plan" && std::wstring_view(argv[1]) != L"validate"))
        {
            std::cerr << "NEONFORGE offline resources\n  nf_resources plan|validate <project.json "
                         "or ->\n";
            return 2;
        }
        bool streamed = std::wstring_view(argv[2]) == L"-";
        if (_setmode(_fileno(stdout), _O_BINARY) == -1 ||
            (streamed && _setmode(_fileno(stdin), _O_BINARY) == -1))
            throw std::runtime_error("cannot configure binary resource streams");
        std::ifstream file;
        if (!streamed)
            file.open(std::filesystem::path(argv[2]), std::ios::binary);
        auto &input = streamed ? std::cin : static_cast<std::istream &>(file);
        if (!input)
            throw std::runtime_error("cannot read resource project");
        std::string source(1048577, '\0');
        input.read(source.data(), static_cast<std::streamsize>(source.size()));
        source.resize(static_cast<size_t>(input.gcount()));
        if (input.bad())
            throw std::runtime_error("resource project read failure");
        if (source.size() > 1048576)
            throw std::invalid_argument("resource project exceeds 1 MiB");
        auto result = nf::resources::compose_resources(nf::parse_json_exact(source));
        if (std::wstring_view(argv[1]) == L"validate")
            result = nf::Json(nf::Json::Object{
                {"valid", nf::Json(true)},
                {"cache_key", result.at("cache_key")},
                {"resources", nf::Json(std::to_string(result.at("resources").object().size()))},
                {"engine_adapter_ready", nf::Json(false)},
                {"game_tested", nf::Json(false)}});
        auto encoded = nf::to_json(result);
        std::cout << encoded << '\n';
        std::cout.flush();
        if (!std::cout)
            throw std::runtime_error("resource output failed");
        return 0;
    }
    catch (const nf::ParseError &e)
    {
        std::cerr << "NEONFORGE resources at byte " << e.offset << ": " << diagnostic(e.what())
                  << '\n';
        return 1;
    }
    catch (const std::bad_variant_access &)
    {
        std::cerr << "NEONFORGE resources: wrong JSON kind\n";
        return 1;
    }
    catch (const std::logic_error &e)
    {
        std::cerr << "NEONFORGE resources: " << diagnostic(e.what()) << '\n';
        return 1;
    }
    catch (const std::exception &e)
    {
        std::cerr << "NEONFORGE resources: " << diagnostic(e.what()) << '\n';
        return 3;
    }
}
