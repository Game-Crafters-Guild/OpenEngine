#include "Prefabs/PrefabThumbnailHandler.h"

namespace GameEngine::Editor
{

std::string PrefabThumbnailHandler::GetOrRequest(const std::filesystem::path& /*assetPath*/,
                                                 int /*desiredSize*/,
                                                 std::function<void(const std::string& relPath)> /*onReady*/,
                                                 bool /*staticModelListThumbnail*/)
{
    return kGlyphPath;
}

} // namespace GameEngine::Editor
