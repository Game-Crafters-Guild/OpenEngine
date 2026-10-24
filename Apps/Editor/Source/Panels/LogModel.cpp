#include "Panels/LogModel.h"

#include "Logger/LogSink.h"
#include "ScriptEditor/SourceLocation.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <iterator>
#include <optional>
#include <string_view>

namespace GameEngine {

namespace {

void AppendFileAndLine(std::string& out, std::string_view file, int line)
{
    out += file;
    if (line > 0)
    {
        out += ':';
        out += std::to_string(line);
    }
}

// The logger records its own calls at the top of a trace; they are never where the entry came from.
constexpr std::string_view kLoggerSymbolPrefix = "Logger::";

const Logger::BacktraceFrame* FindFirstFrameOutsideLogger(const std::vector<Logger::BacktraceFrame>& frames)
{
    for (const Logger::BacktraceFrame& frame : frames)
    {
        if (frame.File.empty() || frame.Line <= 0)
            continue;
        if (std::string_view(frame.Symbol).starts_with(kLoggerSymbolPrefix))
            continue;
        return &frame;
    }
    return nullptr;
}

} // namespace

LogModel::Entry LogModel::MakeEntry(const Logger::LogMessage& message)
{
    Entry entry;
    entry.Level = message.Level;
    entry.Text = message.Message;
    entry.Timestamp = message.Timestamp;
    entry.SourceFile = message.SourceFile;
    entry.SourceLine = message.SourceLine;
    entry.Function = message.Function;
    entry.Backtrace = message.Backtrace;
    return entry;
}

LogModel::AppendResult LogModel::Append(std::deque<Entry>& batch)
{
    AppendResult result;
    if (batch.empty())
        return result;

    const size_t previousVisible = GetVisibleCount();
    const size_t firstNew = m_Entries.size();
    for (Entry& entry : batch)
    {
        entry.Sequence = m_NextSequence++;
        m_Entries.push_back(std::move(entry));
    }
    batch.clear();
    result.Appended = m_Entries.size() - firstNew;

    if (m_Entries.size() > kDefaultMaxEntries)
    {
        const size_t excess = m_Entries.size() - kDefaultMaxEntries;
        m_Entries.erase(m_Entries.begin(), m_Entries.begin() + static_cast<std::ptrdiff_t>(excess));
        result.Trimmed = true;
        Refilter();
        result.AppendedVisible = 0;
        for (size_t index = m_Entries.size() - std::min(result.Appended, m_Entries.size()); index < m_Entries.size();
             ++index)
        {
            if (PassesSeverityFilter(m_Entries[index]) && MatchesSearch(m_Entries[index]))
                ++result.AppendedVisible;
        }
        return result;
    }

    CountAndFilter(firstNew);
    result.AppendedVisible = GetVisibleCount() - previousVisible;
    return result;
}

void LogModel::Clear()
{
    m_Entries.clear();
    m_VisibleIndices.clear();
    m_SearchCounts.fill(0);
}

std::deque<LogModel::Entry>::const_iterator LogModel::FindEntryIterator(uint64_t sequence) const
{
    // Sequences increase in arrival order and entries are only dropped from the front.
    const auto it = std::lower_bound(m_Entries.begin(), m_Entries.end(), sequence,
                                     [](const Entry& entry, uint64_t value) { return entry.Sequence < value; });
    return it != m_Entries.end() && it->Sequence == sequence ? it : m_Entries.end();
}

const LogModel::Entry* LogModel::FindEntry(uint64_t sequence) const
{
    const auto it = FindEntryIterator(sequence);
    return it != m_Entries.end() ? &*it : nullptr;
}

std::optional<size_t> LogModel::FindVisibleRow(uint64_t sequence) const
{
    const auto entry = FindEntryIterator(sequence);
    if (entry == m_Entries.end())
        return std::nullopt;
    const size_t index = static_cast<size_t>(entry - m_Entries.begin());
    if (!IsFiltered())
        return index;
    const auto it = std::lower_bound(m_VisibleIndices.begin(), m_VisibleIndices.end(), index);
    if (it == m_VisibleIndices.end() || *it != index)
        return std::nullopt;
    return static_cast<size_t>(it - m_VisibleIndices.begin());
}

void LogModel::SetSeverityShown(Severity severity, bool shown)
{
    bool& slot = m_Shown[static_cast<size_t>(severity)];
    if (slot == shown)
        return;
    slot = shown;
    Refilter();
}

bool LogModel::IsSeverityFilterActive() const
{
    return std::find(m_Shown.begin(), m_Shown.end(), false) != m_Shown.end();
}

void LogModel::SetSearch(std::string_view text)
{
    if (text == m_Search)
        return;
    m_Search = text;
    Refilter();
}

void LogModel::SetSearchField(SearchField field)
{
    if (field == m_SearchField)
        return;
    m_SearchField = field;
    Refilter();
}

bool LogModel::IsFiltered() const
{
    return !m_Search.empty() || IsSeverityFilterActive();
}

size_t LogModel::GetVisibleCount() const
{
    return IsFiltered() ? m_VisibleIndices.size() : m_Entries.size();
}

const LogModel::Entry* LogModel::GetVisibleEntry(size_t row) const
{
    if (!IsFiltered())
        return GetEntry(row);
    return row < m_VisibleIndices.size() ? GetEntry(m_VisibleIndices[row]) : nullptr;
}

std::string LogModel::FormatVisibleLines() const
{
    std::string out;
    const size_t count = GetVisibleCount();
    for (size_t row = 0; row < count; ++row)
    {
        out += FormatLine(*GetVisibleEntry(row));
        out += '\n';
    }
    return out;
}

LogModel::Severity LogModel::SeverityOf(Logger::LogLevel level)
{
    switch (level)
    {
    case Logger::LogLevel::Trace:
    case Logger::LogLevel::Debug:
        return Severity::Debug;
    case Logger::LogLevel::Warning:
        return Severity::Warning;
    case Logger::LogLevel::Error:
    case Logger::LogLevel::Critical:
        return Severity::Error;
    case Logger::LogLevel::Info:
    default:
        return Severity::Info;
    }
}

std::string LogModel::FormatTime(const Entry& entry)
{
    return Logger::FormatTimestamp(entry.Timestamp);
}

const std::string& LogModel::FormatLine(const Entry& entry)
{
    if (entry.LineCache.empty())
    {
        entry.LineCache.reserve(entry.Text.size() + 32);
        entry.LineCache = "[";
        entry.LineCache += Logger::FormatTimestamp(entry.Timestamp);
        entry.LineCache += "] [";
        entry.LineCache += Logger::LogLevelToString(entry.Level);
        entry.LineCache += "] ";
        entry.LineCache += entry.Text;
    }
    return entry.LineCache;
}

std::string LogModel::FormatSource(const Entry& entry)
{
    if (entry.SourceFile.empty())
        return {};
    std::string out;
    AppendFileAndLine(out, entry.SourceFile, entry.SourceLine);
    if (!entry.Function.empty())
    {
        out += "  ";
        out += entry.Function;
    }
    return out;
}

std::string LogModel::FormatDetails(const Entry& entry)
{
    std::string out = FormatLine(entry);
    out += '\n';

    const std::string source = FormatSource(entry);
    if (!source.empty())
    {
        out += "Source: ";
        out += source;
        out += '\n';
    }

    if (!entry.Backtrace || entry.Backtrace->empty())
        return out;
    out += "Stack trace:\n";
    const std::vector<Logger::BacktraceFrame>& frames = *entry.Backtrace;
    for (size_t index = 0; index < frames.size(); ++index)
    {
        const Logger::BacktraceFrame& frame = frames[index];
        out += '[';
        out += std::to_string(index);
        out += "] ";
        if (!frame.Module.empty())
        {
            out += frame.Module;
            out += '!';
        }
        out += frame.Symbol.empty() ? std::string_view("<unknown>") : std::string_view(frame.Symbol);
        if (!frame.File.empty())
        {
            out += "  ";
            AppendFileAndLine(out, frame.File, frame.Line);
        }
        out += '\n';
    }
    return out;
}

std::optional<LogModel::SourceToOpen> LogModel::FindSourceToOpen(const Entry& entry)
{
    if (const std::optional<Editor::SourceLocation> named = Editor::FindSourceLocationInLogLine(entry.Text))
        return SourceToOpen{named->Path.string(), static_cast<int>(named->Line)};
    if (!entry.SourceFile.empty() && entry.SourceLine > 0)
        return SourceToOpen{entry.SourceFile, entry.SourceLine};
    if (!entry.Backtrace)
        return std::nullopt;
    if (const Logger::BacktraceFrame* frame = FindFirstFrameOutsideLogger(*entry.Backtrace))
        return SourceToOpen{frame->File, frame->Line};
    return std::nullopt;
}

bool LogModel::PassesSeverityFilter(const Entry& entry) const
{
    return IsSeverityShown(SeverityOf(entry.Level));
}

bool LogModel::MatchesSearch(const Entry& entry) const
{
    if (m_Search.empty())
        return true;
    switch (m_SearchField)
    {
    case SearchField::Message:
        return ContainsIgnoreCase(entry.Text, m_Search);
    case SearchField::Level:
        return ContainsIgnoreCase(Logger::LogLevelToString(entry.Level), m_Search);
    case SearchField::Source:
    {
        std::string source;
        AppendFileAndLine(source, entry.SourceFile, entry.SourceLine);
        return ContainsIgnoreCase(source, m_Search);
    }
    case SearchField::Function:
        return ContainsIgnoreCase(entry.Function, m_Search);
    case SearchField::All:
    default:
        return ContainsIgnoreCase(FormatLine(entry), m_Search) || ContainsIgnoreCase(entry.SourceFile, m_Search) ||
               ContainsIgnoreCase(entry.Function, m_Search);
    }
}

void LogModel::Refilter()
{
    m_VisibleIndices.clear();
    m_SearchCounts.fill(0);
    CountAndFilter(0);
}

void LogModel::CountAndFilter(size_t firstIndex)
{
    const bool filtered = IsFiltered();
    for (size_t index = firstIndex; index < m_Entries.size(); ++index)
    {
        const Entry& entry = m_Entries[index];
        if (!MatchesSearch(entry))
            continue;
        ++m_SearchCounts[static_cast<size_t>(SeverityOf(entry.Level))];
        if (filtered && PassesSeverityFilter(entry))
            m_VisibleIndices.push_back(index);
    }
}

} // namespace GameEngine
