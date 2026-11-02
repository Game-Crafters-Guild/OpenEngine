#include "Types/ByteBuffer.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

using GameEngine::ByteBuffer;

namespace
{

static_assert(!std::is_copy_constructible_v<ByteBuffer>, "a buffer has one owner");
static_assert(std::is_convertible_v<const ByteBuffer&, std::span<const std::uint8_t>>);

std::size_t ViewedSize(std::span<const std::uint8_t> bytes)
{
    return bytes.size();
}

TEST(ByteBuffer, HoldsTheBytesItsOwnerWritesAndViewsAllOfThem)
{
    ByteBuffer buffer(5);
    for (std::size_t i = 0; i < buffer.Size(); ++i)
        buffer.Data()[i] = static_cast<std::uint8_t>(10 + i);

    const std::span<const std::uint8_t> view = buffer;
    ASSERT_EQ(view.size(), 5u);
    EXPECT_EQ(view.data(), buffer.Data());
    for (std::size_t i = 0; i < view.size(); ++i)
        EXPECT_EQ(view[i], 10 + i);
    EXPECT_EQ(ViewedSize(buffer), 5u);
}

TEST(ByteBuffer, MovingHandsTheBytesOverAndLeavesTheSourceEmpty)
{
    ByteBuffer source(3);
    source.Data()[0] = 7;
    const std::uint8_t* bytes = source.Data();

    ByteBuffer constructed(std::move(source));
    EXPECT_EQ(constructed.Data(), bytes);
    EXPECT_EQ(constructed.Size(), 3u);
    EXPECT_EQ(source.Size(), 0u);
    EXPECT_EQ(source.begin(), source.end());

    ByteBuffer assigned(8);
    assigned = std::move(constructed);
    EXPECT_EQ(assigned.Data(), bytes);
    EXPECT_EQ(assigned.Size(), 3u);
    EXPECT_EQ(assigned.Data()[0], 7);
    EXPECT_EQ(constructed.Size(), 0u);
}

} // namespace
