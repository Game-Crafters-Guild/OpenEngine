#include "PageStreaming/PageAddress.h"

#include "PageStore/PageStoreSerialization.h"

namespace GameEngine::PageStreaming
{
namespace
{

// Spreads the 32 bits of `value` over the even bits of a 64-bit word.
uint64 SpreadBits(uint32 value)
{
    uint64 bits = value;
    bits = (bits | (bits << 16u)) & 0x0000FFFF0000FFFFull;
    bits = (bits | (bits << 8u)) & 0x00FF00FF00FF00FFull;
    bits = (bits | (bits << 4u)) & 0x0F0F0F0F0F0F0F0Full;
    bits = (bits | (bits << 2u)) & 0x3333333333333333ull;
    bits = (bits | (bits << 1u)) & 0x5555555555555555ull;
    return bits;
}

// Gathers the even bits of `bits` into 32 bits: the inverse of SpreadBits.
uint32 GatherBits(uint64 bits)
{
    bits &= 0x5555555555555555ull;
    bits = (bits | (bits >> 1u)) & 0x3333333333333333ull;
    bits = (bits | (bits >> 2u)) & 0x0F0F0F0F0F0F0F0Full;
    bits = (bits | (bits >> 4u)) & 0x00FF00FF00FF00FFull;
    bits = (bits | (bits >> 8u)) & 0x0000FFFF0000FFFFull;
    bits = (bits | (bits >> 16u)) & 0x00000000FFFFFFFFull;
    return static_cast<uint32>(bits);
}

} // namespace

uint64 PageMortonCode(uint32 x, uint32 z)
{
    return SpreadBits(x) | (SpreadBits(z) << 1u);
}

PageAddress PageOfMortonCode(uint8 face, uint8 level, uint64 code)
{
    return PageAddress{face, level, GatherBits(code), GatherBits(code >> 1u)};
}

} // namespace GameEngine::PageStreaming
