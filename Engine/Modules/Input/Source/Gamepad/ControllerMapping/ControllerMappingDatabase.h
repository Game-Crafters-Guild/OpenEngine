#pragma once

#include "Gamepad/ControllerMapping/ControllerMapping.h"

#include "Types/FlatMap.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::Input
{

/**
 * @brief The entries of the community controller mapping database (SDL_GameControllerDB) for one
 * platform, found by USB vendor and product id. Parse once off the frame thread; a lookup does not
 * allocate. The entries do not move after the parse, so a found pointer lives as long as the database.
 */
class ControllerMappingDatabase
{
  public:
    /// A line of the database text that the parse refused.
    struct RefusedLine
    {
        int LineNumber = 0;
        std::string Reason;
    };

    /// Parses `text` (one entry per line: GUID, name, then control:source fields) and keeps the
    /// lines whose platform field names `platform`. Comments, empty lines and other platforms are
    /// skipped. A malformed line for `platform` is refused and recorded with the reason.
    static ControllerMappingDatabase Parse(std::string_view text, std::string_view platform);

    /// Parses the copy of the database that the build compiles into the Input module
    /// (Data/gamecontrollerdb.txt).
    static ControllerMappingDatabase ParseShipped(std::string_view platform);

    /// @return The first entry for the device, or nullptr when the database has none.
    const ControllerMapping* Find(std::uint16_t vendorId, std::uint16_t productId) const;

    const std::vector<RefusedLine>& RefusedLines() const { return m_RefusedLines; }
    /// The number of devices the database can match.
    std::size_t EntryCount() const { return m_Entries.size(); }

  private:
    std::vector<ControllerMapping> m_Entries;
    // Vendor id in the high 16 bits, product id in the low 16 bits, to the index in m_Entries.
    FlatMap<std::uint32_t, std::uint32_t> m_EntryByDevice;
    std::vector<RefusedLine> m_RefusedLines;
};

} // namespace GameEngine::Input
