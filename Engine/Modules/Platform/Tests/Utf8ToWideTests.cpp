#if defined(_WIN32)

#include "Utf8ToWide_Win32.h"

#include <gtest/gtest.h>

using GameEngine::Platform::Utf8ToWide;

// A two-byte sequence and a four-byte one: widening each byte instead of decoding it turns the
// first into two units and the second into four.
TEST(Utf8ToWide, DecodesMultiByteSequences)
{
    EXPECT_EQ(Utf8ToWide("caf\xC3\xA9.html"), std::wstring(L"caf\u00E9.html"));
    EXPECT_EQ(Utf8ToWide("\xF0\x9F\x8E\xAE"), std::wstring(L"\xD83C\xDFAE"));
    EXPECT_EQ(Utf8ToWide(""), std::wstring());
}

#endif // _WIN32
