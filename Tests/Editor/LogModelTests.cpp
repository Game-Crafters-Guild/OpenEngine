// The Log panel's model: what Copy puts on the clipboard for a selected entry, the count beside
// each severity toggle under a search, the source an entry opens, and that an entry keeps its
// identity when older entries are dropped.

#include <gtest/gtest.h>

#include "Logger/LogSink.h"
#include "Panels/LogModel.h"

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using GameEngine::LogModel;
using Logger::BacktraceFrame;
using Logger::LogLevel;

namespace
{

LogModel::Entry MakeEntry(LogLevel level, std::string text)
{
    LogModel::Entry entry;
    entry.Level = level;
    entry.Text = std::move(text);
    entry.Timestamp = std::chrono::system_clock::now();
    return entry;
}

LogModel::AppendResult AppendOne(LogModel& model, LogModel::Entry entry)
{
    std::deque<LogModel::Entry> batch;
    batch.push_back(std::move(entry));
    return model.Append(batch);
}

} // namespace

TEST(LogModel, CopyHoldsTheLineTheSourceAndEveryFrame)
{
    LogModel::Entry entry = MakeEntry(LogLevel::Error, "Shader failed to compile:\nline 12: unknown identifier");
    entry.SourceFile = "Engine/Source/Rendering/ShaderCache.cpp";
    entry.SourceLine = 214;
    entry.Function = "ShaderCache::Compile";
    auto frames = std::make_shared<std::vector<BacktraceFrame>>();
    frames->push_back({0, "Engine.dll", "ShaderCache::Compile", "ShaderCache.cpp", 214});
    frames->push_back({0, "", "", "", 0});
    entry.Backtrace = frames;

    const std::string expected = LogModel::FormatLine(entry) +
                                 "\n"
                                 "Source: Engine/Source/Rendering/ShaderCache.cpp:214  ShaderCache::Compile\n"
                                 "Stack trace:\n"
                                 "[0] Engine.dll!ShaderCache::Compile  ShaderCache.cpp:214\n"
                                 "[1] <unknown>\n";
    EXPECT_EQ(LogModel::FormatDetails(entry), expected);
    EXPECT_NE(LogModel::FormatLine(entry).find("[ERROR] Shader failed to compile:\nline 12: unknown identifier"),
              std::string::npos)
        << "the line carries the whole message, every line of it";
}

TEST(LogModel, CopyOfAnEntryWithoutSourceIsTheLine)
{
    const LogModel::Entry entry = MakeEntry(LogLevel::Info, "Scene loaded");
    EXPECT_EQ(LogModel::FormatDetails(entry), LogModel::FormatLine(entry) + "\n");
    EXPECT_TRUE(LogModel::FormatSource(entry).empty());
}

TEST(LogModel, EachSeverityCountsTheEntriesTheSearchMatchesWhetherShownOrNot)
{
    LogModel model;
    AppendOne(model, MakeEntry(LogLevel::Trace, "texture streamed"));
    AppendOne(model, MakeEntry(LogLevel::Debug, "texture cooked"));
    AppendOne(model, MakeEntry(LogLevel::Info, "texture imported"));
    AppendOne(model, MakeEntry(LogLevel::Info, "scene saved"));
    AppendOne(model, MakeEntry(LogLevel::Warning, "texture is not a power of two"));
    AppendOne(model, MakeEntry(LogLevel::Error, "texture missing"));
    AppendOne(model, MakeEntry(LogLevel::Critical, "device lost"));

    EXPECT_EQ(model.CountMatchingSearch(LogModel::Severity::Debug), 2u) << "Trace counts as Debug";
    EXPECT_EQ(model.CountMatchingSearch(LogModel::Severity::Error), 2u) << "Critical counts as Error";

    model.SetSearch("TEXTURE");
    model.SetSeverityShown(LogModel::Severity::Info, false);
    EXPECT_EQ(model.CountMatchingSearch(LogModel::Severity::Debug), 2u);
    EXPECT_EQ(model.CountMatchingSearch(LogModel::Severity::Info), 1u) << "a hidden severity still counts its matches";
    EXPECT_EQ(model.CountMatchingSearch(LogModel::Severity::Warning), 1u);
    EXPECT_EQ(model.CountMatchingSearch(LogModel::Severity::Error), 1u);
    EXPECT_EQ(model.GetVisibleCount(), 4u);

    // An entry that arrives under the filter is counted and shown the same way.
    AppendOne(model, MakeEntry(LogLevel::Warning, "texture too large"));
    AppendOne(model, MakeEntry(LogLevel::Info, "texture reloaded"));
    EXPECT_EQ(model.CountMatchingSearch(LogModel::Severity::Warning), 2u);
    EXPECT_EQ(model.CountMatchingSearch(LogModel::Severity::Info), 2u);
    EXPECT_EQ(model.GetVisibleCount(), 5u);
}

TEST(LogModel, TheSourceToOpenIsTheFileAndLineTheEntryCarries)
{
    LogModel::Entry entry = MakeEntry(LogLevel::Warning, "Asset reimported");
    entry.SourceFile = "Engine/Source/Assets/Importer.cpp";
    entry.SourceLine = 88;
    const std::optional<LogModel::SourceToOpen> source = LogModel::FindSourceToOpen(entry);
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(source->File, "Engine/Source/Assets/Importer.cpp");
    EXPECT_EQ(source->Line, 88);
}

TEST(LogModel, TheSourceToOpenIsTheScriptAMessageNamesBeforeTheEntrySource)
{
    LogModel::Entry entry = MakeEntry(LogLevel::Error, "Assets/Scripts/Player.cs(42,7): error CS1002: ; expected");
    entry.SourceFile = "Engine/Source/Scripting/CompileServer.cpp";
    entry.SourceLine = 301;
    const std::optional<LogModel::SourceToOpen> source = LogModel::FindSourceToOpen(entry);
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(source->File, "Assets/Scripts/Player.cs");
    EXPECT_EQ(source->Line, 42);
}

TEST(LogModel, TheSourceToOpenIsTheFirstFrameOutsideTheLoggerWhenTheEntryNamesNoLine)
{
    LogModel::Entry entry = MakeEntry(LogLevel::Error, "Script assembly failed to load");
    auto frames = std::make_shared<std::vector<BacktraceFrame>>();
    frames->push_back({0, "Engine.dll", "Logger::Log::EnqueueString", "Logger.cpp", 360});
    frames->push_back({0, "Engine.dll", "Logger::Log::Error", "Logger.h", 52});
    frames->push_back({0, "Engine.dll", "ScriptManager::LoadAssembly", "", 0});
    frames->push_back({0, "Engine.dll", "ScriptManager::Reload", "ScriptManager.cpp", 1139});
    entry.Backtrace = frames;
    const std::optional<LogModel::SourceToOpen> source = LogModel::FindSourceToOpen(entry);
    ASSERT_TRUE(source.has_value());
    EXPECT_EQ(source->File, "ScriptManager.cpp");
    EXPECT_EQ(source->Line, 1139);
}

TEST(LogModel, AnEntryThatNamesNoLineHasNoSourceToOpen)
{
    LogModel::Entry entry = MakeEntry(LogLevel::Info, "Play mode started");
    EXPECT_FALSE(LogModel::FindSourceToOpen(entry).has_value());
    entry.SourceFile = "Engine/Source/Play.cpp";
    EXPECT_FALSE(LogModel::FindSourceToOpen(entry).has_value()) << "a file without a line is not a place to jump to";
}

TEST(LogModel, TraceAndCriticalObeyTheDebugAndErrorToggles)
{
    LogModel model;
    AppendOne(model, MakeEntry(LogLevel::Trace, "trace"));
    AppendOne(model, MakeEntry(LogLevel::Critical, "critical"));
    AppendOne(model, MakeEntry(LogLevel::Info, "info"));
    model.SetSeverityShown(LogModel::Severity::Debug, false);
    model.SetSeverityShown(LogModel::Severity::Error, false);
    ASSERT_EQ(model.GetVisibleCount(), 1u);
    EXPECT_EQ(model.GetVisibleEntry(0)->Text, "info");
    EXPECT_EQ(model.CountMatchingSearch(LogModel::Severity::Error), 1u) << "a hidden severity still counts";
}

TEST(LogModel, AnEntryTakesTheMessageItsSourceAndItsStackTrace)
{
    Logger::LogMessage message;
    message.Level = LogLevel::Error;
    message.Message = "hello";
    message.Timestamp = std::chrono::system_clock::now();
    message.SetSourceLocation("Foo.cpp", 42, "Bar");
    message.Backtrace = std::make_shared<const std::vector<BacktraceFrame>>(std::vector<BacktraceFrame>(2));
    const LogModel::Entry entry = LogModel::MakeEntry(message);
    EXPECT_EQ(entry.Text, "hello");
    EXPECT_EQ(entry.SourceFile, "Foo.cpp");
    EXPECT_EQ(entry.SourceLine, 42);
    EXPECT_EQ(entry.Function, "Bar");
    ASSERT_NE(entry.Backtrace, nullptr);
    EXPECT_EQ(entry.Backtrace->size(), 2u);

    message.SetSourceLocation(nullptr, 0, nullptr);
    const LogModel::Entry bare = LogModel::MakeEntry(message);
    EXPECT_TRUE(bare.SourceFile.empty());
    EXPECT_TRUE(bare.Function.empty());
}

TEST(LogModel, AnEntryKeepsItsIdentityWhenOlderEntriesAreDropped)
{
    LogModel model;
    AppendOne(model, MakeEntry(LogLevel::Info, "first"));
    AppendOne(model, MakeEntry(LogLevel::Info, "second"));
    const uint64_t selected = model.GetVisibleEntry(1)->Sequence;

    // Fill the model to its maximum, then one more: only "first" is dropped.
    std::deque<LogModel::Entry> fill;
    for (size_t index = 0; index + 2 < LogModel::kDefaultMaxEntries; ++index)
        fill.push_back(MakeEntry(LogLevel::Info, "filler"));
    ASSERT_FALSE(model.Append(fill).Trimmed);
    ASSERT_TRUE(AppendOne(model, MakeEntry(LogLevel::Info, "next")).Trimmed);
    ASSERT_EQ(model.GetVisibleCount(), LogModel::kDefaultMaxEntries);
    const LogModel::Entry* found = model.FindEntry(selected);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->Text, "second") << "the selection followed a row index instead of the entry";

    AppendOne(model, MakeEntry(LogLevel::Info, "last"));
    EXPECT_EQ(model.FindEntry(selected), nullptr) << "a dropped entry is gone, not replaced by its neighbor";
}
