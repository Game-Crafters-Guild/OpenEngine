#pragma once

#include <string>

namespace GameEngine::ComponentScanner
{

// A construct in a scanned header the scanner could not read as written: the 1-based line it
// starts on and what was done about it. The caller prefixes the header's path.
struct SourceWarning
{
    int Line = 0;
    std::string Message;
};

} // namespace GameEngine::ComponentScanner
