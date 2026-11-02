#include "data_model.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

int wmain(int argc, wchar_t **argv)
{
    try
    {
        if (argc != 3 ||
            (std::wstring_view(argv[1]) != L"apply" && std::wstring_view(argv[1]) != L"plan" &&
             std::wstring_view(argv[1]) != L"validate"))
        {
            std::cerr << "NEONFORGE offline data\n  nf_data validate|plan|apply <project.json>\n";
            return 2;
        }
        std::ifstream input(std::filesystem::path(argv[2]), std::ios::binary);
        if (!input)
            throw std::invalid_argument("cannot read data project");
        std::string source(1048577, '\0');
        input.read(source.data(), static_cast<std::streamsize>(source.size()));
        source.resize(static_cast<size_t>(input.gcount()));
        if (input.bad() || source.size() > 1048576)
            throw std::invalid_argument("data project read failure or input exceeds 1 MiB");
        auto result = nf::data::apply_project(nf::parse_json_exact(source),
                                              std::wstring_view(argv[1]) == L"plan");
        if (std::wstring_view(argv[1]) == L"validate")
            result = nf::Json(nf::Json::Object{{"valid", nf::Json(true)},
                                               {"revision", result.at("revision")},
                                               {"engine_adapter_ready", nf::Json(false)},
                                               {"game_tested", nf::Json(false)}});
        auto output = nf::to_json(result);
        std::cout << output << '\n';
        std::cout.flush();
        if (!std::cout)
            throw std::runtime_error("data output failed");
        return 0;
    }
    catch (const nf::ParseError &e)
    {
        std::cerr << "NEONFORGE data at byte " << e.offset << ": " << e.what() << '\n';
        return 1;
    }
    catch (const std::bad_variant_access &)
    {
        std::cerr << "NEONFORGE data: wrong JSON kind in project or schema\n";
        return 1;
    }
    catch (const std::exception &e)
    {
        std::cerr << "NEONFORGE data: " << e.what() << '\n';
        return 1;
    }
}
