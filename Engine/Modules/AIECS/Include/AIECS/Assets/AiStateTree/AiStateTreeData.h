#pragma once

#include "AI/StateTree/StateTreeProgram.h"
#include "AIECS/Assets/AiStateTree/AiStateTreeSource.h"
#include "AssetCore/GUID.h"

#include <string>
#include <string_view>

namespace GameEngine::AIECS
{

/// What a loaded state-tree program file (.scribe) holds: the file as the author wrote it and the program it
/// compiles to. The program uses the indices of the source's name lists; the AI registry resolves the names
/// to the registered ids.
struct AiStateTreeData
{
    AiStateTreeSource Source;
    AI::StateTreeProgram Program;
    /// AiStateTreeProgram::StableId: the GUID of the asset that loaded the file. Null for text parsed with no asset.
    GUID StableId;
};

/// Reads a program file's text into the form the author wrote. Refused, with the field and the fix in
/// `outError`: each file that docs/ai/ai-state-trees.mdx says is refused.
bool ParseAiStateTreeSource(std::string_view text, AiStateTreeSource& out, std::string& outError);

/// Writes `source` as program file text in `outText`. Refused, with the fix in `outError`: a float that is not
/// finite. The text reads back to the same source when each name list holds each name once, in first-use order.
bool WriteAiStateTree(const AiStateTreeSource& source, std::string& outText, std::string& outError);

/// Reads a program file's text with ParseAiStateTreeSource and compiles it. Refused, with the fix in
/// `outError`: each refusal of ParseAiStateTreeSource and of CompileAiStateTree.
bool ParseAiStateTree(std::string_view text, AiStateTreeData& out, std::string& outError);

/// Compiles `source.Definition` into `out` with AI::StateTreeProgram::Compile. A refusal names its state with
/// StateRefusalName, so the log, the editor and a game's tools read the same text.
bool CompileAiStateTree(const AiStateTreeSource& source, AI::StateTreeProgram& out, AI::StateTreeCompileError& outError);

/// The title of state `index` of `source` in the editor: its name, or "State <id>" when it has no name.
/// `index` is an index of `source.Definition.States`.
std::string StateTitle(const AiStateTreeSource& source, size_t index);

/// The name of state `index` of `source` in a refusal: "<name> (state <id>)", or "State <id>" when it has no
/// name. `index` is an index of `source.Definition.States`.
std::string StateRefusalName(const AiStateTreeSource& source, size_t index);

} // namespace GameEngine::AIECS
