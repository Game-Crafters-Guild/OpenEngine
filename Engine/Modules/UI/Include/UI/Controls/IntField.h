#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include "UI/Controls/FloatField.h"
#include "UI/Controls/TextField.h"
#include "NumericExpression/NumericExpression.h"
#include "Types/ParseNumber.h"
#include "Types/StringUtils.h"

namespace GameEngine {

// Numeric field for editing a single integer value. Shares FloatField's math
// expression syntax (sin, sqrt, pi, etc.), rounding to the nearest integer on commit.
class IntField : public TextFieldBase<int>
{
public:
    using ValueType = int;

    IntField();

    // Optional inclusive clamp applied whenever the value is set or typed.
    // Call with equal min/max to enforce a fixed value.
    void SetRange(int minVal, int maxVal) { m_Min = minVal; m_Max = maxVal; m_HasRange = true; }
    void ClearRange() { m_HasRange = false; }

    void SetValue(const int& v) override
    {
        TextFieldBase<int>::SetValue(m_HasRange ? std::clamp(v, m_Min, m_Max) : v);
    }

    /// Parses `text` as the field's value. A whole decimal integer is read exactly; any other text is
    /// read as a numeric expression and rounded to the nearest integer. A value past the range of
    /// `int` is clamped to that range. Returns false when the text is not a number.
    static bool TryParseInt(const std::string& text, int& outValue);

protected:
    int ConstrainTypedValue(const int& value) const override
    {
        return m_HasRange ? std::clamp(value, m_Min, m_Max) : value;
    }

private:
    static std::string FormatInt(int v);

    bool m_HasRange = false;
    int  m_Min = 0;
    int  m_Max = 0;
};

inline IntField::IntField()
{
    AddClass("int-field");
	    ConfigureEditorIdentity("int-input-", "int-field-input");
     if (TextInput* editor = GetTextInput())
         editor->AddClass("numeric-field-input");

    SetFilterFunction([](const std::string& raw, bool /*isFinal*/)
    {
        return FilterNumericExpressionInput(raw);
    });

    SetParseFunction([](const std::string& textValue, int& out, bool /*isFinal*/)
    {
        return TryParseInt(textValue, out);
    });

    SetFormatFunction([](int v)
    {
        return FormatInt(v);
    });

    SetEmptyValues(0, 0);

    SetValue(0);
}

inline std::string IntField::FormatInt(int v)
{
    return std::to_string(v);
}

inline bool IntField::TryParseInt(const std::string& text, int& outValue)
{
    constexpr int64_t kMin = std::numeric_limits<int>::min();
    constexpr int64_t kMax = std::numeric_limits<int>::max();
    if (const std::optional<int64_t> whole = ParseInt(TrimWhitespaceView(text)))
    {
        outValue = static_cast<int>(std::clamp(*whole, kMin, kMax));
        return true;
    }
    float f = 0.0f;
    if (!FloatField::TryParseFloat(text, f))
    {
        return false;
    }
    const double rounded = std::round(static_cast<double>(f));
    outValue = static_cast<int>(std::clamp(rounded, static_cast<double>(kMin), static_cast<double>(kMax)));
    return true;
}

} // namespace GameEngine

