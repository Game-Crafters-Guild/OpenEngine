#include <gtest/gtest.h>

#include "Rendering/Utils/CubeLutColorDomain.h"
#include "Rendering/Utils/CubeLutFileParser.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

using namespace GameEngine::Rendering;

namespace
{
// The importer's proposal for a fixture file (Tests/Fixtures/CubeLut), parsed as the asset parses it.
CubeLutColorProposal ProposeFixture(const char* name)
{
    std::ifstream in(std::filesystem::path(GE_CUBE_LUT_FIXTURES_DIR) / name, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    CubeLutParseResult parsed{};
    EXPECT_TRUE(ParseCubeLutFromText(text.str(), parsed) && parsed.Ok) << name << ": " << parsed.Error;
    return ProposeCubeLutColor(parsed);
}

// The proposal for a 0 to 1 table with a title (the input range the fixtures default to).
CubeLutColorProposal ProposeTitle(const char* title)
{
    CubeLutParseResult parsed{};
    parsed.Ok = true;
    parsed.Has3D = true;
    parsed.Size3D = 2;
    parsed.Title = title;
    return ProposeCubeLutColor(parsed);
}
} // namespace

TEST(CubeLutFileParserTest, Parse3DOnly2CubeRedMajorOrder)
{
    const char* text = R"(LUT_3D_SIZE 2
LUT_3D_INPUT_RANGE 0.0 1.0
0.0 0.0 0.0
1.0 0.0 0.0
0.0 1.0 0.0
1.0 1.0 0.0
0.0 0.0 1.0
1.0 0.0 1.0
0.0 1.0 1.0
1.0 1.0 1.0
)";

    CubeLutParseResult r{};
    ASSERT_TRUE(ParseCubeLutFromText(text, r));
    ASSERT_TRUE(r.Ok);
    EXPECT_FALSE(r.Has1D);
    EXPECT_TRUE(r.Has3D);
    EXPECT_EQ(r.Size3D, 2u);
    EXPECT_FLOAT_EQ(r.In3DMin, 0.0f);
    EXPECT_FLOAT_EQ(r.In3DMax, 1.0f);
    ASSERT_EQ(r.Lut3DRgb.size(), 8u * 3u);
    EXPECT_FLOAT_EQ(r.Lut3DRgb[0], 0.0f);
    EXPECT_FLOAT_EQ(r.Lut3DRgb[3], 1.0f);
    EXPECT_FLOAT_EQ(r.Lut3DRgb[21], 1.0f);
    EXPECT_FLOAT_EQ(r.Lut3DRgb[22], 1.0f);
    EXPECT_FLOAT_EQ(r.Lut3DRgb[23], 1.0f);
}

TEST(CubeLutFileParserTest, Parse1DOnlyMinimal)
{
    const char* text = R"(LUT_1D_SIZE 2
0.1 0.2 0.3
0.4 0.5 0.6
)";

    CubeLutParseResult r{};
    ASSERT_TRUE(ParseCubeLutFromText(text, r));
    ASSERT_TRUE(r.Ok);
    EXPECT_TRUE(r.Has1D);
    EXPECT_FALSE(r.Has3D);
    EXPECT_EQ(r.Size1D, 2u);
    ASSERT_EQ(r.Lut1DRgb.size(), 6u);
    EXPECT_FLOAT_EQ(r.Lut1DRgb[0], 0.1f);
    EXPECT_FLOAT_EQ(r.Lut1DRgb[3], 0.4f);
}

TEST(CubeLutFileParserTest, ScalarDomainMinMaxSetsInputRange)
{
    const char* text = R"(DOMAIN_MIN -0.5 -0.5 -0.5
DOMAIN_MAX 1.5 1.5 1.5
LUT_3D_SIZE 2
0.0 0.0 0.0
1.0 0.0 0.0
0.0 1.0 0.0
1.0 1.0 0.0
0.0 0.0 1.0
1.0 0.0 1.0
0.0 1.0 1.0
1.0 1.0 1.0
)";

    CubeLutParseResult r{};
    ASSERT_TRUE(ParseCubeLutFromText(text, r));
    ASSERT_TRUE(r.Ok);
    EXPECT_FLOAT_EQ(r.In3DMin, -0.5f);
    EXPECT_FLOAT_EQ(r.In3DMax, 1.5f);
}

TEST(CubeLutFileParserTest, PerChannelDomainFailsExplicitly)
{
    const char* text = R"(DOMAIN_MIN 0.0 0.1 0.0
DOMAIN_MAX 1.0 1.0 1.0
LUT_3D_SIZE 2
0.0 0.0 0.0
1.0 0.0 0.0
0.0 1.0 0.0
1.0 1.0 0.0
0.0 0.0 1.0
1.0 0.0 1.0
0.0 1.0 1.0
1.0 1.0 1.0
)";

    CubeLutParseResult r{};
    EXPECT_FALSE(ParseCubeLutFromText(text, r));
    EXPECT_FALSE(r.Error.empty());
}

TEST(CubeLutFileParserTest, ParseShaperPlus3DResolveSample)
{
    const char* text = R"(# Sample 3D cube file containing 1D shaper LUT and 3D LUT.
LUT_1D_SIZE 6
LUT_1D_INPUT_RANGE 0.0 1.0
LUT_3D_SIZE 3
LUT_3D_INPUT_RANGE 0.0 1.0
1.0 1.0 1.0
0.8 0.8 0.8
0.6 0.6 0.6
0.4 0.4 0.4
0.2 0.2 0.2
0.0 0.0 0.0
1.0 1.0 1.0
0.5 1.0 1.0
0.0 1.0 1.0
1.0 0.5 1.0
0.5 0.5 1.0
0.0 0.5 1.0
1.0 0.0 1.0
0.5 0.0 1.0
0.0 0.0 1.0
1.0 1.0 0.5
0.5 1.0 0.5
0.0 1.0 0.5
1.0 0.5 0.5
0.5 0.5 0.5
0.0 0.5 0.5
1.0 0.0 0.5
0.5 0.0 0.5
0.0 0.0 0.5
1.0 1.0 0.0
0.5 1.0 0.0
0.0 1.0 0.0
1.0 0.5 0.0
0.5 0.5 0.0
0.0 0.5 0.0
1.0 0.0 0.0
0.5 0.0 0.0
0.0 0.0 0.0
)";

    CubeLutParseResult r{};
    ASSERT_TRUE(ParseCubeLutFromText(text, r));
    ASSERT_TRUE(r.Ok);
    EXPECT_TRUE(r.Has1D);
    EXPECT_TRUE(r.Has3D);
    EXPECT_EQ(r.Size1D, 6u);
    EXPECT_EQ(r.Size3D, 3u);
    EXPECT_EQ(r.Lut1DRgb.size(), 18u);
    EXPECT_EQ(r.Lut3DRgb.size(), 27u * 3u);
    EXPECT_FLOAT_EQ(r.Lut1DRgb[0], 1.0f);
    EXPECT_FLOAT_EQ(r.Lut1DRgb[15], 0.0f);
    EXPECT_FLOAT_EQ(r.Lut3DRgb[0], 1.0f);
    EXPECT_FLOAT_EQ(r.Lut3DRgb[78], 0.0f);
}

TEST(CubeLutFileParserTest, CommentAfterHeaderIsAllowed)
{
    const char* text = R"(LUT_3D_SIZE 2
LUT_3D_INPUT_RANGE 0.0 1.0
0 0 0
# allowed
1 0 0
0 1 0
1 1 0
0 0 1
1 0 1
0 1 1
1 1 1
)";

    CubeLutParseResult r{};
    EXPECT_TRUE(ParseCubeLutFromText(text, r));
    EXPECT_TRUE(r.Ok);
}

TEST(CubeLutFileParserTest, Utf8BomIsAccepted)
{
    const char* text = "\xEF\xBB\xBFLUT_1D_SIZE 2\n0.1 0.2 0.3\n0.4 0.5 0.6\n";

    CubeLutParseResult r{};
    ASSERT_TRUE(ParseCubeLutFromText(text, r));
    ASSERT_TRUE(r.Ok);
    EXPECT_TRUE(r.Has1D);
    EXPECT_EQ(r.Size1D, 2u);
}

// The parser keeps the TITLE line's text, the proposal's main evidence.
TEST(CubeLutFileParserTest, TitleIsKept)
{
    CubeLutParseResult r{};
    ASSERT_TRUE(ParseCubeLutFromText("TITLE \"Film look \"\nLUT_1D_SIZE 2\n0 0 0\n1 1 1\n", r));
    EXPECT_EQ(r.Title, "Film look ");
}

// One test per row of the proposal rule (CubeLutColorDomain.h), on fixture files.
TEST(CubeLutColorProposal, TitleNamingLogCProposesLogC3)
{
    const CubeLutColorProposal p = ProposeFixture("arri_logc_look.cube");
    EXPECT_EQ(p.InputEncoding, CubeLutInputEncoding::ArriLogC3) << p.Reason;
}

TEST(CubeLutColorProposal, TitleNamingACEScctProposesACEScct)
{
    const CubeLutColorProposal p = ProposeFixture("acescct_look.cube");
    EXPECT_EQ(p.InputEncoding, CubeLutInputEncoding::ACEScct) << p.Reason;
}

TEST(CubeLutColorProposal, TitleNamingDaVinciIntermediateProposesIt)
{
    const CubeLutColorProposal p = ProposeFixture("davinci_intermediate_grade.cube");
    EXPECT_EQ(p.InputEncoding, CubeLutInputEncoding::DaVinciIntermediate) << p.Reason;
}

TEST(CubeLutColorProposal, TitleNamingCineonToRec709ProposesCineon)
{
    const CubeLutColorProposal p = ProposeFixture("cineon_print_to_rec709.cube");
    EXPECT_EQ(p.InputEncoding, CubeLutInputEncoding::Cineon) << p.Reason;
}

TEST(CubeLutColorProposal, TitleSayingLinearProposesLinear)
{
    const CubeLutColorProposal p = ProposeFixture("display_linear_bake.cube");
    EXPECT_EQ(p.InputEncoding, CubeLutInputEncoding::Linear) << p.Reason;
}

TEST(CubeLutColorProposal, InputRangeAboveOneProposesLinear)
{
    const CubeLutColorProposal p = ProposeFixture("hdr_range.cube");
    EXPECT_EQ(p.InputEncoding, CubeLutInputEncoding::Linear) << p.Reason;
    EXPECT_EQ(p.Reason, "the input range reaches 16");
}

TEST(CubeLutColorProposal, UnitRangeWithNothingNamedProposesSrgb)
{
    const CubeLutColorProposal p = ProposeFixture("unnamed_display.cube");
    EXPECT_EQ(p.InputEncoding, CubeLutInputEncoding::Rec709SRGB) << p.Reason;
}

// Titles the rule once misread. A word that merely contains an encoding's name names nothing.
TEST(CubeLutColorProposal, WordsContainingAnEncodingsNameNameNothing)
{
    for (const char* title : {"Warrior teal", "Carribean sunset", "Nonlinear contrast"})
    {
        const CubeLutColorProposal p = ProposeTitle(title);
        EXPECT_EQ(p.InputEncoding, CubeLutInputEncoding::Rec709SRGB) << title << ": " << p.Reason;
    }
}

// A title "X to Y" converts from X: the input side is the encoding before "to".
TEST(CubeLutColorProposal, AConversionTitleNamesItsInputBeforeTo)
{
    EXPECT_EQ(ProposeTitle("sRGB to Linear").InputEncoding, CubeLutInputEncoding::Rec709SRGB);
    EXPECT_EQ(ProposeTitle("Rec709 to LogC").InputEncoding, CubeLutInputEncoding::Rec709SRGB);
    EXPECT_EQ(ProposeTitle("LogC to Rec709").InputEncoding, CubeLutInputEncoding::ArriLogC3);
    EXPECT_EQ(ProposeTitle("Print film to LogC").InputEncoding, CubeLutInputEncoding::Rec709SRGB)
        << "an output side alone names no input";
}

// An encoding the engine does not have is named in the reason, not read as a neighboring curve.
TEST(CubeLutColorProposal, AnUnsupportedEncodingIsNamedAsNotSupported)
{
    for (const auto& [title, name] : {std::pair{"Alexa LogC4", "LogC4"}, std::pair{"S-Log3 look", "S-Log3"},
                                      std::pair{"ACEScc grade", "ACEScc"}})
    {
        const CubeLutColorProposal p = ProposeTitle(title);
        EXPECT_EQ(p.InputEncoding, CubeLutInputEncoding::Rec709SRGB) << title;
        EXPECT_NE(p.Reason.find(std::string(name) + ", which is not supported"), std::string::npos) << p.Reason;
    }
}

// A display LUT (Rec.709 / sRGB, or Linear over 0 to 1) belongs at Finish; a log curve, or Linear
// over a scene range, reads scene values and belongs at Grade.
TEST(CubeLutColorProposal, DisplayInputIsSrgbOrLinearUpToOne)
{
    EXPECT_TRUE(CubeLutExpectsDisplayInput(CubeLutInputEncoding::Rec709SRGB, 1.0f));
    EXPECT_TRUE(CubeLutExpectsDisplayInput(CubeLutInputEncoding::Linear, 1.0f));
    EXPECT_FALSE(CubeLutExpectsDisplayInput(CubeLutInputEncoding::Linear, 16.0f));
    for (const CubeLutInputEncoding log : {CubeLutInputEncoding::ArriLogC3, CubeLutInputEncoding::DaVinciIntermediate,
                                           CubeLutInputEncoding::ACEScct, CubeLutInputEncoding::Cineon})
        EXPECT_FALSE(CubeLutExpectsDisplayInput(log, 1.0f)) << CubeLutInputEncodingLabel(log);
}
