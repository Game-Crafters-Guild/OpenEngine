#include "UI/StyleUtil.h"

#include "UI/UIElement.h"

#include <string>
#include <string_view>

namespace GameEngine::UIUtil
{
namespace
{
// Assets/defaults.css gives this class `display: none`.
constexpr const char* kHiddenClass = "hidden";
} // namespace

std::string NormalizeCssUrlPath(std::string path)
{
    constexpr std::string_view kAssetScheme = "asset:";
    if (path.starts_with(kAssetScheme))
        path.erase(0, kAssetScheme.size());
    return path;
}

void SetShown(UIElement& element, bool shown)
{
    if (shown)
        element.RemoveClass(kHiddenClass);
    else
        element.AddClass(kHiddenClass);
}

void SetClassPresent(UIElement& element, const std::string& className, bool present)
{
    if (present)
        element.AddClass(className);
    else
        element.RemoveClass(className);
}

} // namespace GameEngine::UIUtil
