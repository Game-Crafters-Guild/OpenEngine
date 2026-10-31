#pragma once

#include "Types/StringId.h"

#include <cstddef>
#include <string_view>

namespace GameEngine::UI
{
/** Stores constant text and its compile-time StringId for an element ID or class.
 * UIElement retains its own text copy when the name is assigned.
 */
class LiteralName
{
public:
    consteval explicit LiteralName(std::string_view text)
        : m_Text(text), m_Hash(HashStringId(text)) {}

    constexpr std::string_view GetText() const noexcept { return m_Text; }
    constexpr StringId GetHash() const noexcept { return m_Hash; }

private:
    std::string_view m_Text;
    StringId m_Hash;
};

/** Creates a name with its compile-time hash, preserving all literal bytes. */
consteval LiteralName operator""_ui(const char* text, std::size_t length)
{
    return LiteralName{std::string_view{text, length}};
}
}
