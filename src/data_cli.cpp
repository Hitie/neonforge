#include "data_model.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

namespace
{
std::string read_source(const wchar_t *path, size_t limit)
{
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input)
        throw std::invalid_argument("cannot read data input");
    std::string source(limit + 1, '\0');
    input.read(source.data(), static_cast<std::streamsize>(source.size()));
    source.resize(static_cast<size_t>(input.gcount()));
    if (input.bad() || source.size() > limit)
        throw std::invalid_argument("data input read failure or byte limit exceeded");
    return source;
}
} // namespace
int wmain(int argc, wchar_t **argv)
{
    try
    {
        std::wstring_view mode = argc > 1 ? argv[1] : L"";
        if (!((argc == 3 && (mode == L"apply" || mode == L"plan" || mode == L"validate" ||
                             mode == L"checkpoint" || mode == L"restore")) ||
              (argc == 4 && mode == L"reload")))
        {
            std::cerr << "NEONFORGE offline data\n  nf_data validate|plan|apply|checkpoint "
                         "<project.json>\n"
                         "  nf_data restore <checkpoint.json>\n"
                         "  nf_data reload <checkpoint.json> <candidate-project.json>\n";
            return 2;
        }
        bool accepted = true;
        auto source =
            read_source(argv[2], (mode == L"restore" || mode == L"reload" ? 32u : 1u) * 1048576);
        nf::Json result;
        if (mode == L"restore" || mode == L"reload")
        {
            nf::data::ProjectSession session;
            if (!session.restore(source))
                throw std::invalid_argument(std::string(session.error()));
            if (mode == L"reload")
            {
                auto generation = session.generation();
                accepted = session.reload(read_source(argv[3], 1048576));
                result = nf::Json(
                    nf::Json::Object{{"accepted", nf::Json(accepted)},
                                     {"changed", nf::Json(generation != session.generation())},
                                     {"generation", nf::Json(std::to_string(session.generation()))},
                                     {"error", nf::Json(std::string(session.error()))},
                                     {"state", session.snapshot().json()}});
            }
            else
                result = session.snapshot().json();
        }
        else if (mode == L"checkpoint")
            result = nf::data::build_project(nf::parse_json_exact(source))->checkpoint();
        else
            result = nf::data::apply_project(nf::parse_json_exact(source), mode == L"plan");
        if (mode == L"validate")
            result = nf::Json(nf::Json::Object{{"valid", nf::Json(true)},
                                               {"revision", result.at("revision")},
                                               {"engine_adapter_ready", nf::Json(false)},
                                               {"game_tested", nf::Json(false)}});
        auto output = nf::to_json(result);
        std::cout << output << '\n';
        std::cout.flush();
        if (!std::cout)
            throw std::runtime_error("data output failed");
        return accepted ? 0 : 1;
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
