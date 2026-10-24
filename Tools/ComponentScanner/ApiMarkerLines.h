#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::ComponentScanner
{

// The script API markers (ApiScanner.h), as one raw header line carries them.
enum class Marker
{
    Script,
    Handle,
    Event,
    Try,
    Context,
    EngineThread,
    AnyThread,
    Flags,
};

// True for the markers that say what a declaration is: @ge-script, @ge-script-handle and
// @ge-script-event.
bool IsPrimary(Marker marker);

// The script markers on one raw line, and what kind of line it is.
struct LineMarkers
{
    std::vector<Marker> Markers;
    std::optional<std::string> EventPayload; // the word after @ge-script-event
    std::vector<std::string> Unknown;        // "@ge-..." tokens that are not a marker
    bool CommentOnly = false;                // nothing but a // comment
    bool HasCode = false;                    // code before the comment
    std::optional<std::string> Doc;          // the text of a /// line with no marker
    bool DocMarker = false;                  // a /// line whose text starts with "@ge-"

    // True when the line is evidence of a script marker: a marker, a `///` marker line, or a
    // word that starts with @ge-script and is not a marker (a misspelling). A field marker of
    // another kind, such as @ge-tooltip, is not.
    bool HasScriptMarker() const;
};

// The preprocessor group a line sits in: Dead inside the live branch of `#if 0`, else the
// innermost conditional group's first line and directive (0 and empty outside every group).
struct LineCondition
{
    bool Dead = false;
    int GroupLine = 0;
    std::string GroupText;
};

namespace ApiMarkerLines
{

// The markers, "@ge-script, @ge-script-handle, ...", for a refusal's fix.
std::string MarkerList();

// The markers of one raw line. A marker comment starts with its markers
// ("// @ge-script @ge-context"); prose that names a marker further into a comment carries none.
LineMarkers ReadLine(std::string_view line);

// The preprocessor group of every line of a header, 1-based (index 0 unused). An include
// guard (`#ifndef X` and `#define X` as the first two directives, closed by the last
// `#endif`) is no group.
std::vector<LineCondition> ConditionsByLine(const std::vector<std::string>& lines);

// The markers of every line of a header, 1-based (index 0 empty). A line in a dead `#if 0`
// branch, or whose // sits inside a /* */ block comment, carries none; the second reads as a
// comment-only line.
std::vector<LineMarkers> MarkersByLine(const std::vector<std::string>& lines, const std::vector<LineCondition>& conditions);

// The text of `///` lines as one doc string: lines joined with a space, a blank `///` line a
// paragraph break ("\n").
std::string JoinDoc(const std::vector<std::string>& lines);

// The doc of the declaration on 1-based `declLine`: the `///` lines of the comment run directly
// above it, joined (JoinDoc). Plain `//` lines (markers) in the run are skipped; a blank line or
// code ends it.
std::string DocAbove(const std::vector<std::string>& lines, int declLine);

} // namespace ApiMarkerLines

} // namespace GameEngine::ComponentScanner
