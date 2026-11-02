// Parser contract for ParseEditorCommandLine, focused on --debug-port.
//
// Why this file exists: a mistyped --debug-port used to be swallowed silently and
// the editor bound the DEFAULT debug port — which is very often another
// developer's (or the user's own) running editor. So the interesting cases here
// are all the ways a value can be wrong: every one of them must leave debugPort
// UNSET and land a verbatim entry in unrecognizedArgs for the caller to warn
// about. Ported from the adversarial review harness for 08906153d.

#include "Startup/EditorCommandLine.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace GameEngine::Editor::Startup
{
namespace
{
// argv[0] is the exe name, as in a real launch — the parser skips it.
EditorCommandLineArgs Parse(std::vector<const char*> args)
{
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("Editor.exe"));
    for (const char* a : args)
        argv.push_back(const_cast<char*>(a));
    return ParseEditorCommandLine(static_cast<int>(argv.size()), argv.data());
}
} // namespace

// --- Accepted forms ---

TEST(EditorCommandLineTests, DebugPort_SpaceForm)
{
    const auto r = Parse({"--debug-port", "9971"});
    ASSERT_TRUE(r.debugPort.has_value());
    EXPECT_EQ(*r.debugPort, 9971u);
    EXPECT_TRUE(r.unrecognizedArgs.empty());
}

TEST(EditorCommandLineTests, DebugPort_EqualsForm)
{
    const auto r = Parse({"--debug-port=9971"});
    ASSERT_TRUE(r.debugPort.has_value());
    EXPECT_EQ(*r.debugPort, 9971u);
    EXPECT_TRUE(r.unrecognizedArgs.empty());
}

TEST(EditorCommandLineTests, DebugPort_MaxPortAccepted)
{
    const auto r = Parse({"--debug-port", "65535"});
    ASSERT_TRUE(r.debugPort.has_value());
    EXPECT_EQ(*r.debugPort, 65535u);
    EXPECT_TRUE(r.unrecognizedArgs.empty());
}

// strtol's leading '+' leniency is accepted rather than special-cased: "+9971"
// names the same port, and rejecting it would be surprising, not safer.
TEST(EditorCommandLineTests, DebugPort_LeadingPlusAccepted)
{
    const auto r = Parse({"--debug-port", "+9971"});
    ASSERT_TRUE(r.debugPort.has_value());
    EXPECT_EQ(*r.debugPort, 9971u);
}

TEST(EditorCommandLineTests, DebugPort_LastOccurrenceWins)
{
    const auto r = Parse({"--debug-port", "9971", "--debug-port", "80"});
    ASSERT_TRUE(r.debugPort.has_value());
    EXPECT_EQ(*r.debugPort, 80u);
}

// --- Rejected values: never fall through to the default port, always warn ---

TEST(EditorCommandLineTests, DebugPort_ZeroRejected)
{
    const auto r = Parse({"--debug-port", "0"});
    EXPECT_FALSE(r.debugPort.has_value());
    EXPECT_EQ(r.unrecognizedArgs, (std::vector<std::string>{"--debug-port 0"}));
}

TEST(EditorCommandLineTests, DebugPort_AboveMaxRejected)
{
    const auto r = Parse({"--debug-port", "65536"});
    EXPECT_FALSE(r.debugPort.has_value());
    EXPECT_EQ(r.unrecognizedArgs, (std::vector<std::string>{"--debug-port 65536"}));
}

// "80x" must not be read as 80: the parse has to consume the whole token.
TEST(EditorCommandLineTests, DebugPort_TrailingJunkRejected)
{
    const auto r = Parse({"--debug-port", "80x"});
    EXPECT_FALSE(r.debugPort.has_value());
    EXPECT_EQ(r.unrecognizedArgs, (std::vector<std::string>{"--debug-port 80x"}));
}

TEST(EditorCommandLineTests, DebugPort_HexRejected)
{
    const auto r = Parse({"--debug-port", "0x270F"});
    EXPECT_FALSE(r.debugPort.has_value());
    EXPECT_EQ(r.unrecognizedArgs, (std::vector<std::string>{"--debug-port 0x270F"}));
}

TEST(EditorCommandLineTests, DebugPort_OverflowRejected)
{
    const auto r = Parse({"--debug-port", "99999999999999999999"});
    EXPECT_FALSE(r.debugPort.has_value());
    EXPECT_EQ(r.unrecognizedArgs,
              (std::vector<std::string>{"--debug-port 99999999999999999999"}));
}

TEST(EditorCommandLineTests, DebugPort_EqualsFormWithEmptyValueRejected)
{
    const auto r = Parse({"--debug-port="});
    EXPECT_FALSE(r.debugPort.has_value());
    EXPECT_EQ(r.unrecognizedArgs, (std::vector<std::string>{"--debug-port="}));
}

// The warning names the value so "--debug-port" alone and "--debug-port 0" are
// distinguishable — they fail for different reasons.
TEST(EditorCommandLineTests, DebugPort_MissingValueNamedInWarning)
{
    const auto r = Parse({"--debug-port"});
    EXPECT_FALSE(r.debugPort.has_value());
    EXPECT_EQ(r.unrecognizedArgs, (std::vector<std::string>{"--debug-port <missing value>"}));
}

// A dash-prefixed next token is NOT claimed as the value: "--debug-port
// --project X" must leave --project intact rather than eating it.
TEST(EditorCommandLineTests, DebugPort_DoesNotSwallowFollowingFlag)
{
    const auto r = Parse({"--debug-port", "--project", "C:/tmp"});
    EXPECT_FALSE(r.debugPort.has_value());
    EXPECT_EQ(r.unrecognizedArgs, (std::vector<std::string>{"--debug-port <missing value>"}));
    EXPECT_TRUE(r.projectRoot.has_value()) << "--project must survive the rejected port";
}

// Negative values look like flags, so the value is not claimed and the "-1"
// token is then reported on its own — two warnings, both accurate.
TEST(EditorCommandLineTests, DebugPort_NegativeReportsBothTokens)
{
    const auto r = Parse({"--debug-port", "-1"});
    EXPECT_FALSE(r.debugPort.has_value());
    EXPECT_EQ(r.unrecognizedArgs,
              (std::vector<std::string>{"--debug-port <missing value>", "-1"}));
}

// --- Unrecognized-argument reporting for everything else ---

TEST(EditorCommandLineTests, UnknownFlagReported)
{
    const auto r = Parse({"--unknown-flag"});
    EXPECT_EQ(r.unrecognizedArgs, (std::vector<std::string>{"--unknown-flag"}));
}

// Engine flags are consumed by ParseEngineArgs from the same argv, so this
// parser must not report them — known or not.
TEST(EditorCommandLineTests, KnownEngineFlagNotReported)
{
    const auto r = Parse({"--engine-assets=X"});
    EXPECT_TRUE(r.unrecognizedArgs.empty());
}

TEST(EditorCommandLineTests, UnknownEngineFlagStaysSilentHere)
{
    const auto r = Parse({"--engine-frobnicate"});
    EXPECT_TRUE(r.unrecognizedArgs.empty());
}

TEST(EditorCommandLineTests, KnownEditorFlagNotReported)
{
    const auto r = Parse({"-logfile", "x.log"});
    EXPECT_TRUE(r.unrecognizedArgs.empty());
    EXPECT_TRUE(r.logFile.has_value());
}

// The log file flag is spelled -logfile, and only that. A near miss must be
// reported rather than quietly ignored: a run that believes it asked for a log
// path and got the default one instead reads every count from the wrong file.
TEST(EditorCommandLineTests, DoubleDashLogFileSpellingIsReported)
{
    const auto r = Parse({"--log-file", "x.log"});
    EXPECT_FALSE(r.logFile.has_value());
    EXPECT_EQ(r.unrecognizedArgs, (std::vector<std::string>{"--log-file"}));
}

TEST(EditorCommandLineTests, BareDashIgnored)
{
    const auto r = Parse({"-"});
    EXPECT_TRUE(r.unrecognizedArgs.empty());
}

TEST(EditorCommandLineTests, BareDoubleDashReported)
{
    const auto r = Parse({"--"});
    EXPECT_EQ(r.unrecognizedArgs, (std::vector<std::string>{"--"}));
}

// Positional arguments are not this parser's business — no warning.
TEST(EditorCommandLineTests, PositionalArgumentStaysSilent)
{
    const auto r = Parse({"positional.scene"});
    EXPECT_TRUE(r.unrecognizedArgs.empty());
}

// --- --session-label ---
//
// The label says what an editor session is FOR, so the parser's whole job is to pass it
// through untouched. The interesting cases are the ones where it must NOT invent one.

TEST(EditorCommandLineTests, SessionLabel_SpaceForm)
{
    const auto r = Parse({"--session-label", "forced-LOD0-arm"});
    ASSERT_TRUE(r.sessionLabel.has_value());
    EXPECT_EQ(*r.sessionLabel, "forced-LOD0-arm");
    EXPECT_TRUE(r.unrecognizedArgs.empty());
}

TEST(EditorCommandLineTests, SessionLabel_EqualsForm)
{
    const auto r = Parse({"--session-label=forced-LOD0-arm"});
    ASSERT_TRUE(r.sessionLabel.has_value());
    EXPECT_EQ(*r.sessionLabel, "forced-LOD0-arm");
    EXPECT_TRUE(r.unrecognizedArgs.empty());
}

// One argv entry may already contain spaces (the shell quoted it); the parser must not
// tokenize or trim what the launcher deliberately passed as a single label.
TEST(EditorCommandLineTests, SessionLabel_SpacesPreservedVerbatim)
{
    const auto r = Parse({"--session-label", "LOD measurement: heavy arm, do not kill"});
    ASSERT_TRUE(r.sessionLabel.has_value());
    EXPECT_EQ(*r.sessionLabel, "LOD measurement: heavy arm, do not kill");
}

TEST(EditorCommandLineTests, SessionLabel_AbsentByDefault)
{
    const auto r = Parse({"--project", "X"});
    EXPECT_FALSE(r.sessionLabel.has_value());
}

// Same trap as --debug-port: a missing value must not eat the following flag. Here the
// consequence would be a silently dropped --project, which is how a lane committed into
// another lane's project once already.
TEST(EditorCommandLineTests, SessionLabel_MissingValueDoesNotSwallowNextFlag)
{
    const auto r = Parse({"--session-label", "--project", "X"});
    EXPECT_FALSE(r.sessionLabel.has_value());
    ASSERT_TRUE(r.projectRoot.has_value());
    EXPECT_EQ(r.unrecognizedArgs, (std::vector<std::string>{"--session-label <missing value>"}));
}

TEST(EditorCommandLineTests, SessionLabel_TrailingFlagWithNoValueReported)
{
    const auto r = Parse({"--session-label"});
    EXPECT_FALSE(r.sessionLabel.has_value());
    EXPECT_EQ(r.unrecognizedArgs, (std::vector<std::string>{"--session-label <missing value>"}));
}

// A label may legitimately begin with '-'; the =form is how it gets through, taken
// verbatim rather than mistaken for a flag.
TEST(EditorCommandLineTests, SessionLabel_EqualsFormAllowsLeadingDash)
{
    const auto r = Parse({"--session-label=-baseline-arm"});
    ASSERT_TRUE(r.sessionLabel.has_value());
    EXPECT_EQ(*r.sessionLabel, "-baseline-arm");
    EXPECT_TRUE(r.unrecognizedArgs.empty());
}

TEST(EditorCommandLineTests, SessionLabel_EmptyEqualsValueReported)
{
    const auto r = Parse({"--session-label="});
    EXPECT_FALSE(r.sessionLabel.has_value());
    EXPECT_EQ(r.unrecognizedArgs, (std::vector<std::string>{"--session-label="}));
}

// --- --window ---

TEST(EditorCommandLineTests, Window_SizeAndPosition)
{
    const auto r = Parse({"--window", "1920x1080+40+-60"});
    ASSERT_TRUE(r.windowRect.has_value());
    EXPECT_EQ(*r.windowRect, (Mathematics::RectInt{40, -60, 1920, 1080}));
    EXPECT_TRUE(r.windowHasPosition);
    EXPECT_TRUE(r.refusedArgs.empty());
}

TEST(EditorCommandLineTests, Window_EqualsFormSizeOnly)
{
    const auto r = Parse({"--window=1280x720"});
    ASSERT_TRUE(r.windowRect.has_value());
    EXPECT_EQ(r.windowRect->Width, 1280);
    EXPECT_EQ(r.windowRect->Height, 720);
    EXPECT_FALSE(r.windowHasPosition);
    EXPECT_TRUE(r.refusedArgs.empty());
}

// A malformed value is refused, not ignored: the editor must not open maximized
// when a recording asked for a fixed size.
TEST(EditorCommandLineTests, Window_MalformedValuesRefused)
{
    for (const char* value : {"1920x1080+40", "1920x1080+40,40", "0x1080", "1920*1080", "1920x1080+40+40+1"})
    {
        const auto r = Parse({"--window", value});
        EXPECT_FALSE(r.windowRect.has_value()) << value;
        ASSERT_EQ(r.refusedArgs.size(), 1u) << value;
        EXPECT_NE(r.refusedArgs[0].find("<width>x<height>"), std::string::npos) << value;
    }
}

TEST(EditorCommandLineTests, Window_MissingValueDoesNotSwallowNextFlag)
{
    const auto r = Parse({"--window", "--project", "X"});
    EXPECT_FALSE(r.windowRect.has_value());
    ASSERT_TRUE(r.projectRoot.has_value());
    EXPECT_EQ(r.refusedArgs.size(), 1u);
}

} // namespace GameEngine::Editor::Startup
