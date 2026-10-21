#include "Editor/Registries/BuiltInStatusBarItems.h"

#include "Assets/TexturePreparingStatusItem.h"

namespace GameEngine::Editor
{

void RegisterBuiltInStatusBarItems(StatusBarItemRegistry& registry)
{
    RegisterTexturePreparingStatusItem(registry);
}

} // namespace GameEngine::Editor
