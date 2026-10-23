#pragma once

#include "UI/Controls/Label.h"
#include "UI/UIElement.h"

#include <memory>
#include <string>

namespace GameEngine::Editor
{

/// Adds a new `T` carrying `cssClass` as the last child of `parent`, which owns it.
template <typename T>
T* AddDayCycleChild(UIElement& parent, const char* cssClass)
{
    auto child = std::make_unique<T>();
    child->AddClass(cssClass);
    T* raw = child.get();
    parent.AddChild(std::move(child));
    return raw;
}

/// Adds a label showing `text`, carrying `cssClass`, as the last child of `parent`.
inline Label* AddDayCycleText(UIElement& parent, const std::string& text, const char* cssClass)
{
    Label* label = AddDayCycleChild<Label>(parent, cssClass);
    label->SetText(text);
    return label;
}

} // namespace GameEngine::Editor
