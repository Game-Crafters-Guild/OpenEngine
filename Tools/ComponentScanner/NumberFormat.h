#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace GameEngine::ComponentScanner::NumberFormat
{

// Parses a decimal number the way a marker payload is written: optional leading sign,
// digits with an optional point, an optional exponent ("0.5", ".5", "5.", "-1.5e-7", "+5").
// Surrounding whitespace is allowed; anything else ("0.0f", "0x10", "1,5") is rejected.
// Infinities and NaN parse; the caller rejects them.
std::optional<double> ParseDouble(std::string_view text);

// The shortest digit string that reads back as the same double, laid out with a decimal
// point when the decimal exponent is at least -4 and below the greater of the digit count and
// 17, and as "d.dddE+XX" (sign always written, at least two exponent digits) otherwise:
// "0.5", "100000000000000", "1E-05", "1.2345678901234568E+17". Negative zero is "-0".
std::string ShortestRoundTrip(double value);

enum class LiteralStatus
{
    NotALiteral,
    Ok,
    OutOfRange,
};

// A C++ number literal in its one spelling, or why it has none.
struct CanonicalNumber
{
    LiteralStatus Status = LiteralStatus::NotALiteral;
    std::string Text;
};

// An integer literal ("0xFFu", "-12", "255") as decimal text with no suffix ("255"); a
// magnitude past 64 bits is OutOfRange.
CanonicalNumber CanonicalInteger(std::string_view literal);

// A float literal ("1.0f", "1.f", "1", "0x10") as the shortest text that reads back as the
// same float ("1", "16"); a value a float cannot hold is OutOfRange.
CanonicalNumber CanonicalFloat(std::string_view literal);

// A double literal ("2.50", "1") as ShortestRoundTrip writes it ("2.5", "1"); a value a double
// cannot hold is OutOfRange. A literal with the float suffix is not a double literal.
CanonicalNumber CanonicalDouble(std::string_view literal);

} // namespace GameEngine::ComponentScanner::NumberFormat
