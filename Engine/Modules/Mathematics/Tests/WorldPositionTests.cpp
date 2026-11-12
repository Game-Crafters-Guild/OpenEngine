#include "Mathematics/WorldPosition.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

using GameEngine::Mathematics::kWorldSectorSize;
using GameEngine::Mathematics::Vector3;
using GameEngine::Mathematics::Vector3Int;
using GameEngine::Mathematics::WorldPosition;

namespace
{

// Sectors at 300 km and at 1e9 m, each sign.
constexpr int32_t kFarSectors[] = {293, -293, 976562, -976562};

// The terrain decode's 32-bit split (CBTDeepDecode.h, DecodeDeep): floor(x / 1024 + 0.5) in floats.
int32_t DecodeSplit(float meters)
{
    constexpr float kInvSector = 1.0f / static_cast<float>(kWorldSectorSize);
    return static_cast<int32_t>(std::floor(meters * kInvSector + 0.5f));
}

void ExpectLocalInRange(float local)
{
    EXPECT_GE(local, -512.0f);
    EXPECT_LT(local, 512.0f);
}

} // namespace

TEST(WorldPositionTests, FromSectorThenToSectorGivesBackTheSectorAndTheLocal)
{
    const float locals[] = {-512.0f, -0.25f, 0.0f, 123.456f, std::nextafter(512.0f, 0.0f)};
    for (const int32_t sector : kFarSectors)
    {
        for (const float local : locals)
        {
            const Vector3Int inSector(sector, -sector, sector);
            const Vector3 inLocal(local, local, local);
            Vector3Int outSector;
            Vector3 outLocal;
            WorldPosition::FromSector(inSector, inLocal).ToSector(outSector, outLocal);
            EXPECT_EQ(outSector, inSector) << "sector " << sector << " local " << local;
            EXPECT_EQ(outLocal.x, inLocal.x);
            EXPECT_EQ(outLocal.y, inLocal.y);
            EXPECT_EQ(outLocal.z, inLocal.z);
        }
    }
}

TEST(WorldPositionTests, ToSectorThenFromSectorIsWithinTwoToTheMinusSixteenMeters)
{
    const double kBound = std::ldexp(1.0, -16);
    for (const int32_t sector : kFarSectors)
    {
        const double base = static_cast<double>(sector) * kWorldSectorSize;
        // A stride that is not a binary fraction, over three sectors.
        for (int32_t step = -2000; step <= 2000; ++step)
        {
            const double x = base + static_cast<double>(step) * 0.7681234567;
            const WorldPosition position{x, -x, x};
            Vector3Int outSector;
            Vector3 outLocal;
            position.ToSector(outSector, outLocal);
            ExpectLocalInRange(outLocal.x);
            ExpectLocalInRange(outLocal.y);
            const WorldPosition back = WorldPosition::FromSector(outSector, outLocal);
            EXPECT_LE(std::abs(back.X - position.X), kBound) << "x " << x;
            EXPECT_LE(std::abs(back.Y - position.Y), kBound) << "y " << -x;
        }
    }
}

TEST(WorldPositionTests, ALocalThatRoundsToHalfASectorMovesToTheNextSector)
{
    // 512 - 2^-20 m is a double whose local rounds to 512.0f in 32 bits.
    const double justBelowHalf = 512.0 - std::ldexp(1.0, -20);
    Vector3Int outSector;
    Vector3 outLocal;
    WorldPosition{justBelowHalf, -justBelowHalf - kWorldSectorSize, justBelowHalf}.ToSector(outSector, outLocal);
    EXPECT_EQ(outSector, Vector3Int(1, -1, 1));
    EXPECT_EQ(outLocal.x, -512.0f);
    EXPECT_EQ(outLocal.y, -512.0f);
    EXPECT_LE(std::abs(WorldPosition::FromSector(outSector, outLocal).X - justBelowHalf), std::ldexp(1.0, -16));
}

TEST(WorldPositionTests, ToSectorEqualsTheDecodeSplitForThirtyTwoBitPositions)
{
    for (const int32_t sector : kFarSectors)
    {
        const float center = static_cast<float>(static_cast<double>(sector) * kWorldSectorSize);
        // Every half-sector tie and the floats on each side of it, over three sectors.
        for (int32_t half = -3; half <= 3; ++half)
        {
            const float tie = center + static_cast<float>(half) * 512.0f;
            for (const float x : {std::nextafter(tie, -INFINITY), tie, std::nextafter(tie, INFINITY), tie + 0.3f * 512.0f})
            {
                Vector3Int outSector;
                Vector3 outLocal;
                WorldPosition::FromVector3(Vector3(x, x, x)).ToSector(outSector, outLocal);
                EXPECT_EQ(outSector.x, DecodeSplit(x)) << "x " << x;
            }
        }
    }

    // 299,519.99 m is not a 32-bit float: the double split gives 292, the decode's float of it 293.
    const double nearTie = 299519.99;
    Vector3Int outSector;
    Vector3 outLocal;
    WorldPosition{nearTie, 0.0, 0.0}.ToSector(outSector, outLocal);
    EXPECT_EQ(outSector.x, 292);
    EXPECT_EQ(DecodeSplit(static_cast<float>(nearTie)), 293);
}

TEST(WorldPositionTests, OffsetsAreComputedInDoubles)
{
    const WorldPosition far{1.0e9, -1.0e9, 3.0e8};
    const WorldPosition moved = far + Vector3(0.25f, -0.5f, 0.125f);
    EXPECT_EQ(moved, (WorldPosition{1.0e9 + 0.25, -1.0e9 - 0.5, 3.0e8 + 0.125}));
    const Vector3 offset = moved - far;
    EXPECT_EQ(offset.x, 0.25f);
    EXPECT_EQ(offset.y, -0.5f);
    EXPECT_EQ(offset.z, 0.125f);
}
