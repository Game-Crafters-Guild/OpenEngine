#pragma once

#include "ECS/Component/ComponentFieldRegistry.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace GameEngine::Editor
{
// Formats captured values without reading or modifying the live document.
class HistoryValueFormatter
{
public:
    static std::string DescribeComponent(ECS::ComponentTypeId typeId, std::span<const std::uint8_t> before,
                                          std::span<const std::uint8_t> after);
    static std::string DescribeMaterial(std::span<const std::uint8_t> before, std::span<const std::uint8_t> after,
                                       const std::filesystem::path& path);
};
} // namespace GameEngine::Editor
