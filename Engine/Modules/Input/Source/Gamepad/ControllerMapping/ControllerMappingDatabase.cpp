#include "Gamepad/ControllerMapping/ControllerMappingDatabase.h"

#include "Types/ParseNumber.h"
#include "Types/StringUtils.h"

#include <format>
#include <optional>

namespace GameEngine::Input
{
namespace
{

// Data/gamecontrollerdb.txt, embedded by the build (cmake/EmbedTextFile.cmake).
#include "ControllerMappingText.h"

constexpr std::string_view kPlatformKey = "platform";
constexpr std::size_t kGuidHexDigits = 32;
// SDL's GUID for any XInput pad. The gamepad backends read those pads without a mapping.
constexpr std::string_view kXInputGuid = "xinput";

// The database's control names. A name not listed here (misc1, paddle1, touchpad) has no
// GamepadButton or GamepadAxis, so its field is skipped.
struct ButtonName
{
    std::string_view Name;
    GamepadButton Button;
};
constexpr ButtonName kButtonNames[] = {
    {"a", GamepadButton::A},
    {"b", GamepadButton::B},
    {"x", GamepadButton::X},
    {"y", GamepadButton::Y},
    {"leftshoulder", GamepadButton::LeftBumper},
    {"rightshoulder", GamepadButton::RightBumper},
    {"back", GamepadButton::Back},
    {"start", GamepadButton::Start},
    {"guide", GamepadButton::Guide},
    {"leftstick", GamepadButton::LeftThumb},
    {"rightstick", GamepadButton::RightThumb},
    {"dpup", GamepadButton::DpadUp},
    {"dpright", GamepadButton::DpadRight},
    {"dpdown", GamepadButton::DpadDown},
    {"dpleft", GamepadButton::DpadLeft},
};

struct AxisName
{
    std::string_view Name;
    GamepadAxis Axis;
};
constexpr AxisName kAxisNames[] = {
    {"leftx", GamepadAxis::LeftX},
    {"lefty", GamepadAxis::LeftY},
    {"rightx", GamepadAxis::RightX},
    {"righty", GamepadAxis::RightY},
    {"lefttrigger", GamepadAxis::LeftTrigger},
    {"righttrigger", GamepadAxis::RightTrigger},
};

std::uint32_t DeviceKey(std::uint16_t vendorId, std::uint16_t productId)
{
    return (static_cast<std::uint32_t>(vendorId) << 16) | productId;
}

std::optional<std::uint8_t> ParseSmallIndex(std::string_view digits)
{
    const std::optional<std::uint64_t> value = ParseUnsigned(digits);
    if (!value || *value > 0xFF)
        return std::nullopt;
    return static_cast<std::uint8_t>(*value);
}

// "b3", "a2", "+a2", "-a2", "a2~" or "h0.4".
std::optional<ControllerMapping::Source> ParseSource(std::string_view token)
{
    ControllerMapping::Source source;
    if (!token.empty() && (token.front() == '+' || token.front() == '-'))
    {
        source.Range = token.front() == '+' ? ControllerMapping::Source::AxisRange::Positive
                                            : ControllerMapping::Source::AxisRange::Negative;
        token.remove_prefix(1);
    }
    if (token.size() < 2)
        return std::nullopt;

    const char kind = token.front();
    token.remove_prefix(1);
    if (kind == 'a')
    {
        source.SourceKind = ControllerMapping::Source::Kind::Axis;
        if (token.back() == '~')
        {
            source.Inverted = true;
            token.remove_suffix(1);
        }
    }
    else if (source.Range != ControllerMapping::Source::AxisRange::Full)
    {
        return std::nullopt;
    }
    else if (kind == 'b')
    {
        source.SourceKind = ControllerMapping::Source::Kind::Button;
    }
    else if (kind == 'h')
    {
        source.SourceKind = ControllerMapping::Source::Kind::Hat;
        const std::size_t dot = token.find('.');
        if (dot == std::string_view::npos)
            return std::nullopt;
        const std::optional<std::uint8_t> mask = ParseSmallIndex(token.substr(dot + 1));
        if (!mask || *mask == 0)
            return std::nullopt;
        source.HatMask = *mask;
        token = token.substr(0, dot);
    }
    else
    {
        return std::nullopt;
    }

    const std::optional<std::uint8_t> index = ParseSmallIndex(token);
    if (!index)
        return std::nullopt;
    source.Index = *index;
    return source;
}

// The SDL joystick GUID as 16 bytes. Vendor and product ids sit little-endian in bytes 4-5 and 8-9
// when bytes 6-7 and 10-11 are zero; another GUID form carries no ids.
bool ParseGuid(std::string_view hex, std::uint16_t& vendorId, std::uint16_t& productId, bool& hasIds)
{
    if (hex.size() != kGuidHexDigits)
        return false;
    std::uint8_t bytes[kGuidHexDigits / 2] = {};
    constexpr int kHexBase = 16;
    for (std::size_t i = 0; i < std::size(bytes); ++i)
    {
        const std::optional<std::uint64_t> byte = ParseUnsigned(hex.substr(i * 2, 2), kHexBase);
        if (!byte)
            return false;
        bytes[i] = static_cast<std::uint8_t>(*byte);
    }
    hasIds = bytes[6] == 0 && bytes[7] == 0 && bytes[10] == 0 && bytes[11] == 0;
    vendorId = static_cast<std::uint16_t>(bytes[4] | (bytes[5] << 8));
    productId = static_cast<std::uint16_t>(bytes[8] | (bytes[9] << 8));
    return true;
}

// Stores `source` for the control `name` ("a", "leftx", "+leftx"). @return False for a half-axis
// prefix on a button name; true for a name with no gamepad control, which is skipped.
bool AssignControl(std::string_view name, const ControllerMapping::Source& source, ControllerMapping& mapping)
{
    char half = 0;
    if (!name.empty() && (name.front() == '+' || name.front() == '-'))
    {
        half = name.front();
        name.remove_prefix(1);
    }
    for (const AxisName& axis : kAxisNames)
    {
        if (axis.Name != name)
            continue;
        const int index = static_cast<int>(axis.Axis);
        ControllerMapping::Source* target = half == '+'   ? &mapping.PositiveHalfAxes[index]
                                            : half == '-' ? &mapping.NegativeHalfAxes[index]
                                                          : &mapping.Axes[index];
        *target = source;
        return true;
    }
    for (const ButtonName& button : kButtonNames)
    {
        if (button.Name != name)
            continue;
        if (half != 0)
            return false;
        mapping.Buttons[static_cast<int>(button.Button)] = source;
        return true;
    }
    return true;
}

// The platform field of a line, or an empty view when the line has none.
std::string_view FindPlatform(const std::vector<std::string_view>& fields)
{
    for (std::size_t i = 2; i < fields.size(); ++i)
    {
        const std::size_t colon = fields[i].find(':');
        if (colon != std::string_view::npos && fields[i].substr(0, colon) == kPlatformKey)
            return fields[i].substr(colon + 1);
    }
    return {};
}

// @return An empty string when `fields` form an entry, otherwise the reason it is refused.
std::string ParseEntry(const std::vector<std::string_view>& fields, ControllerMapping& mapping, bool& hasIds)
{
    const std::string_view name = fields[1];
    if (!ParseGuid(fields[0], mapping.VendorId, mapping.ProductId, hasIds))
        return std::format("'{}': the GUID '{}' is not {} hex digits", name, fields[0], kGuidHexDigits);
    mapping.Name = std::string(name);

    for (std::size_t i = 2; i < fields.size(); ++i)
    {
        const std::string_view field = fields[i];
        if (field.empty())
            continue;
        const std::size_t colon = field.find(':');
        if (colon == std::string_view::npos)
            return std::format("'{}': the field '{}' has no ':'", name, field);
        const std::string_view control = field.substr(0, colon);
        if (control == kPlatformKey)
            continue;
        const std::optional<ControllerMapping::Source> source = ParseSource(field.substr(colon + 1));
        if (!source || !AssignControl(control, *source, mapping))
            return std::format("'{}': the field '{}' is not a control and a source", name, field);
    }
    return {};
}

} // namespace

ControllerMappingDatabase ControllerMappingDatabase::Parse(std::string_view text, std::string_view platform)
{
    ControllerMappingDatabase database;
    int lineNumber = 0;
    for (std::string_view line : Split(text, '\n'))
    {
        ++lineNumber;
        line = TrimWhitespaceView(line);
        if (line.empty() || line.front() == '#')
            continue;
        const std::vector<std::string_view> fields = Split(line, ',');
        if (fields.size() < 2 || fields[0] == kXInputGuid || FindPlatform(fields) != platform)
            continue;

        ControllerMapping mapping;
        bool hasIds = false;
        std::string refusal = ParseEntry(fields, mapping, hasIds);
        if (!refusal.empty())
        {
            database.m_RefusedLines.push_back({lineNumber, std::move(refusal)});
            continue;
        }
        // Only the first entry for a device can match, and an entry without ids never matches.
        const std::uint32_t key = DeviceKey(mapping.VendorId, mapping.ProductId);
        if (!hasIds || database.m_EntryByDevice.Find(key))
            continue;
        database.m_EntryByDevice.InsertOrAssign(key, static_cast<std::uint32_t>(database.m_Entries.size()));
        database.m_Entries.push_back(std::move(mapping));
    }
    return database;
}

ControllerMappingDatabase ControllerMappingDatabase::ParseShipped(std::string_view platform)
{
    // The embedded array ends with a terminating zero that is not part of the text.
    const std::string_view text(reinterpret_cast<const char*>(kControllerMappingText),
                                std::size(kControllerMappingText) - 1);
    return Parse(text, platform);
}

const ControllerMapping* ControllerMappingDatabase::Find(std::uint16_t vendorId, std::uint16_t productId) const
{
    const std::uint32_t* index = m_EntryByDevice.Find(DeviceKey(vendorId, productId));
    return index ? &m_Entries[*index] : nullptr;
}

} // namespace GameEngine::Input
