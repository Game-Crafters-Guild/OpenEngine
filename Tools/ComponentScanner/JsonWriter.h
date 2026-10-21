#pragma once

#include "NumberFormat.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace GameEngine::ComponentScanner
{

// Two-space indented writer with the layout of an indented Utf8JsonWriter: every value on
// its own line, a space after each name's colon, an empty array as "[]".
class JsonWriter
{
  public:
    std::string Text;

    void StartObject(std::string_view name = {})
    {
        BeginValue(name);
        Text += '{';
        m_Depth++;
        m_Empty = true;
    }

    void EndObject()
    {
        End('}');
    }

    void StartArray(std::string_view name)
    {
        BeginValue(name);
        Text += '[';
        m_Depth++;
        m_Empty = true;
    }

    void EndArray()
    {
        End(']');
    }

    void String(std::string_view name, std::string_view value)
    {
        BeginValue(name);
        AppendString(value);
    }

    void Number(std::string_view name, std::int64_t value)
    {
        BeginValue(name);
        Text += std::to_string(value);
    }

    void Number(std::string_view name, double value)
    {
        BeginValue(name);
        Text += NumberFormat::ShortestRoundTrip(value);
    }

    void Boolean(std::string_view name, bool value)
    {
        BeginValue(name);
        Text += value ? "true" : "false";
    }

  private:
    int m_Depth = 0;
    bool m_Empty = true; // no value written yet inside the current container

    void BeginValue(std::string_view name)
    {
        if (m_Depth > 0)
        {
            if (!m_Empty)
                Text += ',';
            Text += '\n';
            Text.append(static_cast<std::size_t>(m_Depth) * 2, ' ');
        }
        m_Empty = false;
        if (!name.empty())
        {
            AppendString(name);
            Text += ": ";
        }
    }

    void End(char close)
    {
        m_Depth--;
        if (!m_Empty)
        {
            Text += '\n';
            Text.append(static_cast<std::size_t>(m_Depth) * 2, ' ');
        }
        Text += close;
        m_Empty = false;
    }

    // Only '"', '\\' and control characters are escaped; everything else, non-ASCII
    // included, is written as is.
    void AppendString(std::string_view value)
    {
        Text += '"';
        for (char c : value)
        {
            switch (c)
            {
            case '"': Text += "\\\""; break;
            case '\\': Text += "\\\\"; break;
            case '\n': Text += "\\n"; break;
            case '\r': Text += "\\r"; break;
            case '\t': Text += "\\t"; break;
            case '\b': Text += "\\b"; break;
            case '\f': Text += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    constexpr char kHex[] = "0123456789abcdef";
                    Text += "\\u00";
                    Text += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                    Text += kHex[static_cast<unsigned char>(c) & 0xF];
                }
                else
                {
                    Text += c;
                }
                break;
            }
        }
        Text += '"';
    }
};

} // namespace GameEngine::ComponentScanner
