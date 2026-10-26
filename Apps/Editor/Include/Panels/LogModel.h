#pragma once

#include "Logger/Backtrace.h"
#include "Logger/LogLevel.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Logger {
struct LogMessage;
}

namespace GameEngine {

/**
 * @brief The entries the Log panel holds, its filters, its counts and the text it copies.
 *
 * No UI: LogView binds a list to it and LogPanel reads the selection, the counts and the copy
 * text from it. Main thread only.
 */
class LogModel {
public:
    static constexpr size_t kDefaultMaxEntries = 1000;

    /// The four severities the panel filters and counts by. Trace reads as Debug, Critical as Error.
    enum class Severity : uint8_t
    {
        Debug,
        Info,
        Warning,
        Error,
    };
    static constexpr size_t kSeverityCount = 4;

    /// The field the search text is matched against.
    enum class SearchField : uint8_t
    {
        All,
        Message,
        Level,
        Source,
        Function,
    };

    struct Entry
    {
        /// Unique within the model and increasing in arrival order: the list's stable item id.
        /// Zero is never assigned.
        uint64_t Sequence = 0;
        Logger::LogLevel Level = Logger::LogLevel::Info;
        std::string Text;
        std::chrono::system_clock::time_point Timestamp;
        std::string SourceFile;
        int SourceLine = 0;
        std::string Function;
        std::shared_ptr<const std::vector<Logger::BacktraceFrame>> Backtrace;
        /// FormatLine, built on first use.
        mutable std::string LineCache;
    };

    /// A place in a source file an entry points at.
    struct SourceToOpen
    {
        std::string File;
        /// 1-based.
        int Line = 0;
    };

    struct AppendResult
    {
        size_t Appended = 0;
        /// Entries appended that the filters show.
        size_t AppendedVisible = 0;
        /// Old entries were dropped to stay within the maximum, so row indices moved.
        bool Trimmed = false;
    };

    /// An entry holding a copy of what `message` carries; its sequence is assigned by Append.
    static Entry MakeEntry(const Logger::LogMessage& message);

    /// Moves every entry out of `batch` to the end, assigns sequences, drops the oldest beyond the maximum.
    AppendResult Append(std::deque<Entry>& batch);
    void Clear();

    /// The entry with this sequence, or null when it was cleared or trimmed.
    const Entry* FindEntry(uint64_t sequence) const;
    /// The row the filters show this entry at, or nothing when it is hidden, cleared or trimmed.
    std::optional<size_t> FindVisibleRow(uint64_t sequence) const;

    void SetSeverityShown(Severity severity, bool shown);
    bool IsSeverityShown(Severity severity) const { return m_Shown[static_cast<size_t>(severity)]; }
    void SetSearch(std::string_view text);
    void SetSearchField(SearchField field);
    /// True when a severity is hidden or a search is typed.
    bool IsFiltered() const;

    /// True when the severity toggles and the search show this entry.
    bool IsShown(const Entry& entry) const { return PassesSeverityFilter(entry) && MatchesSearch(entry); }

    /// The rows the filters show, oldest first.
    size_t GetVisibleCount() const;
    const Entry* GetVisibleEntry(size_t row) const;
    /// Entries of this severity that the search matches, shown or not: the count beside its toggle.
    size_t CountMatchingSearch(Severity severity) const { return m_SearchCounts[static_cast<size_t>(severity)]; }
    /// Every row the filters show, one FormatLine per line.
    std::string FormatVisibleLines() const;

    static Severity SeverityOf(Logger::LogLevel level);
    /// "hh:mm:ss.mmm" in local time: the time column of a row.
    static std::string FormatTime(const Entry& entry);
    /// "[hh:mm:ss.mmm] [LEVEL] message": the row text, and what Copy All copies for each line.
    static const std::string& FormatLine(const Entry& entry);
    /// "file:line  function", or empty when the entry carries no source file.
    static std::string FormatSource(const Entry& entry);
    /// FormatLine, then the source and the stack trace when the entry has them: what Copy puts on the clipboard.
    static std::string FormatDetails(const Entry& entry);
    /// The C# location the message names (a compiler error names the script), else the entry's own
    /// source file and line, else the first stack frame with a file and line outside the logger;
    /// nothing when the entry names no line to open.
    static std::optional<SourceToOpen> FindSourceToOpen(const Entry& entry);

private:
    std::deque<Entry>::const_iterator FindEntryIterator(uint64_t sequence) const;
    const Entry* GetEntry(size_t index) const { return index < m_Entries.size() ? &m_Entries[index] : nullptr; }
    bool IsSeverityFilterActive() const;
    bool PassesSeverityFilter(const Entry& entry) const;
    bool MatchesSearch(const Entry& entry) const;
    void Refilter();
    void CountAndFilter(size_t firstIndex);

    std::deque<Entry> m_Entries;
    uint64_t m_NextSequence = 1;
    std::array<bool, kSeverityCount> m_Shown{true, true, true, true};
    /// As typed; matched without regard to ASCII case.
    std::string m_Search;
    SearchField m_SearchField = SearchField::All;
    /// Entry indices the filters show; used only while IsFiltered().
    std::vector<size_t> m_VisibleIndices;
    std::array<size_t, kSeverityCount> m_SearchCounts{};
};

} // namespace GameEngine
