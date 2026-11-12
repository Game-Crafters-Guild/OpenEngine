#pragma once

#include "AIECS/Assets/AiDataAsset.h"
#include "AIECS/Assets/AiStateTree/AiStateTreeData.h"

namespace GameEngine::AIECS
{

template <>
struct AiDataFormat<AiStateTreeData>
{
    static constexpr AssetType kType = AssetType::AiStateTree;
    static constexpr const char* kNoun = "AI program";
    static constexpr auto Parse = &ParseAiStateTree;
};

/// A state-tree program asset (<name>.scribe). Its data holds the compiled program and its task,
/// condition and input names.
using AiStateTreeAsset = AiDataAsset<AiStateTreeData>;

} // namespace GameEngine::AIECS
