#pragma once

#include "AIECS/Assets/AiDataAsset.h"
#include "AIECS/Assets/AiOptionSetData.h"

namespace GameEngine::AIECS
{

template <>
struct AiDataFormat<AiOptionSetData>
{
    static constexpr AssetType kType = AssetType::AiOptionSet;
    static constexpr const char* kNoun = "AI option set";
    static constexpr auto Parse = &ParseAiOptionSet;
};

/// An option-set asset (<name>.aioptions). AiRegistry::CompileProgram turns its data into a program.
using AiOptionSetAsset = AiDataAsset<AiOptionSetData>;

} // namespace GameEngine::AIECS
