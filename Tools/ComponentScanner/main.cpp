#include "ComponentScanner.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

// Run takes the arguments as UTF-8. Windows hands a narrow main its arguments in the ANSI code
// page, which cannot spell every path, so the wide arguments are converted instead.
#if defined(_WIN32)
int wmain(int argc, wchar_t** argv)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
    {
        const std::u8string text = std::filesystem::path(argv[i]).u8string();
        args.emplace_back(text.begin(), text.end());
    }
    return GameEngine::ComponentScanner::Run(args, std::cout, std::cerr);
}
#else
int main(int argc, char** argv)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    return GameEngine::ComponentScanner::Run(args, std::cout, std::cerr);
}
#endif
