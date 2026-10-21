#include "MarkupContext.h"

#include "Editor/Registries/EditorMarkupRequests.h"
#include "MarkupECS/MarkupRegionOutline.h"
#include "Mathematics/Vector2.h"
#include "Mathematics/Vector3.h"
#include "Spline/SplineUtility.h"
#include "UI/Utf8Helpers.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <charconv>
#include <cmath>
#include <optional>
#include <span>
#include <utility>

namespace GameEngine
{
namespace
{
using nlohmann::json;

constexpr std::string_view kTokenPrefix = "@markup:";
constexpr std::string_view kClose = "</markup>";
// Douglas-Peucker's first tolerance for a path's line, doubled until it fits, as a region's.
constexpr float kPathTolerance = 0.1f;
// Decimals a block writes: outlines, paths and bounds to a tenth of a meter (the outline's own
// tolerance), a volume's center and extents, a circle and heights and lengths to a centimeter,
// a rotation quaternion's components to four, an area to the square meter.
constexpr int kRingDecimals = 1;
constexpr int kMeterDecimals = 2;
constexpr int kRotationDecimals = 4;
// Room kept at a block's and a turn's end for the line that counts what did not fit.
constexpr std::size_t kOverflowLineBytes = 160;

// Trailing punctuation that ends a sentence after a tag token, not part of the tag.
bool IsTrailingPunctuation(char c)
{
    return c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == ')';
}

// `text` cut to at most `bytes` on a character boundary.
std::string CutUtf8(std::string_view text, std::size_t bytes)
{
    std::string cut(text);
    if (cut.size() > bytes)
        cut.resize(Utf8::Prev(cut, bytes + 1));
    return cut;
}

// `c` written as XML text, `quotes` adding the attribute value's quote.
void AppendEscaped(std::string& out, char c, bool quotes)
{
    switch (c)
    {
    case '&': out += "&amp;"; break;
    case '<': out += "&lt;"; break;
    case '>': out += "&gt;"; break;
    case '"':
        out += quotes ? "&quot;" : "\"";
        break;
    default: out += c; break;
    }
}

// `text` as an attribute value: the XML specials escaped, line breaks as spaces.
std::string Attribute(std::string_view text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char c : text)
    {
        if (c == '\n' || c == '\r')
            escaped += ' ';
        else
            AppendEscaped(escaped, c, true);
    }
    return escaped;
}

// `text` as a block's body text: `&`, `<` and `>` escaped, so nothing a user or an agent wrote
// can close the block or open another; line breaks kept unless `oneLine`.
std::string BodyText(std::string_view text, bool oneLine)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char c : text)
    {
        if (oneLine && (c == '\n' || c == '\r'))
            escaped += ' ';
        else
            AppendEscaped(escaped, c, false);
    }
    return escaped;
}

// `text` as body text (BodyText) of at most `bytes` bytes, cut before a character, never inside an
// escaped entity; `kept` is how many of the user's bytes it holds.
std::string CutBodyText(std::string_view text, std::size_t bytes, bool oneLine, std::size_t& kept)
{
    std::string out;
    kept = 0;
    while (kept < text.size())
    {
        const std::size_t next = Utf8::Next(text, kept);
        const std::string piece = BodyText(text.substr(kept, next - kept), oneLine);
        if (out.size() + piece.size() > bytes)
            break;
        out += piece;
        kept = next;
    }
    return out;
}

// `value` with at most `decimals` decimals, trailing zeros dropped; the same text in every locale.
std::string Number(double value, int decimals)
{
    char text[64];
    const std::to_chars_result written = std::to_chars(text, text + sizeof(text), value, std::chars_format::fixed, decimals);
    std::string out(text, written.ptr);
    if (out.find('.') != std::string::npos)
    {
        while (out.back() == '0')
            out.pop_back();
        if (out.back() == '.')
            out.pop_back();
    }
    return out == "-0" ? "0" : out;
}

// One tuple of numbers ("x,y,z", a point or a rotation), from a JSON array.
std::string Tuple(const json& values, int decimals)
{
    std::string out;
    for (const json& value : values)
        out += (out.empty() ? "" : ",") + Number(value.get<double>(), decimals);
    return out;
}

std::vector<Mathematics::Vector2> RingOf(const json& points)
{
    std::vector<Mathematics::Vector2> ring;
    ring.reserve(points.size());
    for (const json& point : points)
        ring.emplace_back(point[0].get<float>(), point[1].get<float>());
    return ring;
}

// "x,z x,z ..." of `ring`, decimated to kMaxShapePoints.
std::string RingText(std::span<const Mathematics::Vector2> ring)
{
    std::string out;
    for (const Mathematics::Vector2& point : MarkupECS::DecimateRegionRing(ring, MarkupContext::kMaxShapePoints))
        out += (out.empty() ? "" : " ") + Number(point.x, kRingDecimals) + "," + Number(point.y, kRingDecimals);
    return out;
}

// "x,y,z ..." of a path's points, simplified by Douglas-Peucker (open, both ends kept) to
// kMaxShapePoints.
std::string PathText(const json& points)
{
    std::vector<Mathematics::Vector3> line;
    for (const json& point : points)
        line.emplace_back(point[0].get<float>(), point[1].get<float>(), point[2].get<float>());
    std::vector<uint32> kept;
    for (float tolerance = kPathTolerance; line.size() > MarkupContext::kMaxShapePoints; tolerance *= 2.0f)
    {
        Spline::SimplifyPolylineIndices(line, tolerance, kept);
        if (kept.size() <= MarkupContext::kMaxShapePoints)
            break;
    }
    std::string out;
    const auto append = [&out](const Mathematics::Vector3& p)
    {
        out += (out.empty() ? "" : " ") + Number(p.x, kRingDecimals) + "," + Number(p.y, kRingDecimals) + "," +
               Number(p.z, kRingDecimals);
    };
    if (kept.empty())
    {
        for (const Mathematics::Vector3& point : line)
            append(point);
    }
    for (const uint32 index : kept)
        append(line[index]);
    return out;
}

// The shape attributes of a region: its outline decimated, the area the outline encloses (what the
// inspector's Outline area shows), the area with its members applied, the bounds and the height;
// or, for knots that cannot outline an area (fewer than three, or edges that cross), the knots and
// the outline's problem with its fix.
std::string RegionAttributes(const json& shape)
{
    const std::string height = " extrudeHeight=\"" + Number(shape.value("extrudeHeight", 0.0), kMeterDecimals) + "\"";
    const std::vector<Mathematics::Vector2> knots = RingOf(shape.value("knots", json::array()));
    const std::optional<std::string> problem = MarkupECS::CheckRegionOutline(knots);
    const json& outline = shape.contains("outline") ? shape["outline"] : json::array();
    if (problem || outline.empty())
    {
        return " knots=\"" + RingText(knots) + "\" problem=\"" +
               Attribute(problem.value_or("The outline encloses no area")) + "\"" + height;
    }
    const json& bounds = shape["bounds"];
    const std::vector<Mathematics::Vector2> ring = RingOf(outline);
    return " outline=\"" + RingText(ring) + "\" area=\"" + Number(MarkupECS::RegionOutlineArea(ring), 0) +
           "\" netArea=\"" + Number(shape.value("area", 0.0), 0) + "\" bounds=\"" +
           Tuple(bounds["min"], kRingDecimals) + " " + Tuple(bounds["max"], kRingDecimals) + "\"" + height;
}

// `text` in double quotes, a quote or a backslash in it written \" or \\, as a quoted title token is.
std::string Quoted(std::string_view text)
{
    std::string quoted = "\"";
    for (const char c : text)
    {
        if (c == '"' || c == '\\')
            quoted += '\\';
        quoted += c;
    }
    return quoted + "\"";
}

// A row's tags (its `tags`, the user's) as a block's `tags` value: each tag Quoted, separated by spaces;
// empty for none. A tag may hold a space, so the quotes are what separate them.
std::string TagsAttribute(const json& row)
{
    std::string tags;
    for (const json& tag : row.value("tags", json::array()))
    {
        if (tag.is_string())
            tags += (tags.empty() ? "" : " ") + Quoted(tag.get<std::string>());
    }
    return Attribute(tags);
}

// One member of a region: its mode, kind, title and tags, and its footprint (an outline
// decimated as the base, or a circle); a deleted one says so.
std::string MemberLine(const json& member)
{
    const std::string mode = Attribute(member.value("mode", "include"));
    if (member.value("deleted", false))
        return "<member mode=\"" + mode + "\" deleted=\"true\"/>";
    std::string line = "<member mode=\"" + mode + "\" kind=\"" + Attribute(member.value("kind", "")) + "\" title=\"" +
                       Attribute(member.value("title", "")) + "\" tags=\"" + TagsAttribute(member) + "\"";
    const json& footprint = member["footprint"];
    if (footprint.contains("radius"))
        line += " center=\"" + Tuple(footprint["center"], kMeterDecimals) + "\" radius=\"" +
                Number(footprint["radius"].get<double>(), kMeterDecimals) + "\"";
    else
        line += " outline=\"" + RingText(RingOf(footprint["outline"])) + "\"";
    return line + "/>";
}

std::string ShapeAttributes(const std::string& kind, const json& shape)
{
    if (kind == "region")
        return RegionAttributes(shape);
    if (kind == "path")
        return " points=\"" + PathText(shape["points"]) + "\" length=\"" +
               Number(shape.value("length", 0.0), kMeterDecimals) + "\"";
    return " shape=\"" + Attribute(shape.value("shape", "box")) + "\" center=\"" +
           Tuple(shape["center"], kMeterDecimals) + "\" halfExtents=\"" + Tuple(shape["halfExtents"], kMeterDecimals) +
           "\" rotation=\"" + Tuple(shape["rotation"], kRotationDecimals) + "\"";
}

// What an entry without text did: "status Requested" for a status change, else its kind
// ("created", "edit").
std::string EntryAction(const json& entry)
{
    const std::string kind = entry.value("kind", "");
    if (kind == "statusChange")
        return "status " + entry.value("status", "");
    return kind;
}

// A thread entry as one line: its author, its time and its text, or what it did for an entry
// without text; cut to kMaxEntryBytes.
std::string EntryLine(const json& entry)
{
    const std::string text = entry.value("text", "");
    // The cap counts the user's bytes; the escaping comes after the cut, so it never splits an entity.
    return BodyText(CutUtf8(entry.value("author", "") + " " + entry.value("time", "") + " " +
                                (text.empty() ? EntryAction(entry) : text),
                            MarkupContext::kMaxEntryBytes),
                    true);
}

// "2 members, 4 entries" for what a block left out; empty when nothing was.
std::string Counted(std::size_t count, const char* one, const char* many)
{
    return count == 0 ? std::string() : std::to_string(count) + " " + (count == 1 ? one : many);
}
} // namespace

std::vector<MarkupToken> MarkupContext::FindTokens(std::string_view prompt)
{
    std::vector<MarkupToken> tokens;
    for (std::size_t at = prompt.find(kTokenPrefix); at != std::string_view::npos;
         at = prompt.find(kTokenPrefix, at + 1))
    {
        if (at > 0 && !std::isspace(static_cast<unsigned char>(prompt[at - 1])) && prompt[at - 1] != '(')
            continue;
        MarkupToken token;
        const std::size_t start = at + kTokenPrefix.size();
        std::size_t end = start;
        if (start < prompt.size() && prompt[start] == '"')
        {
            // A quoted title; \" and \\ stand for a quote and a backslash in it.
            bool closed = false;
            for (end = start + 1; end < prompt.size(); ++end)
            {
                if (prompt[end] == '\\' && end + 1 < prompt.size())
                    token.Title += prompt[++end];
                else if (prompt[end] == '"')
                {
                    closed = true;
                    break;
                }
                else
                    token.Title += prompt[end];
            }
            if (!closed || token.Title.empty())
                continue;
            ++end;
        }
        else
        {
            while (end < prompt.size() && !std::isspace(static_cast<unsigned char>(prompt[end])))
                ++end;
            while (end > start && IsTrailingPunctuation(prompt[end - 1]))
                --end;
            if (end == start)
                continue;
            token.Tag = std::string(prompt.substr(start, end - start));
        }
        token.Text = std::string(prompt.substr(at, end - at));
        bool seen = false;
        for (const MarkupToken& earlier : tokens)
            seen = seen || (earlier.Tag == token.Tag && earlier.Title == token.Title);
        if (!seen)
            tokens.push_back(std::move(token));
    }
    return tokens;
}

std::string MarkupContext::TagOf(const json& row)
{
    const json& tags = row.contains("tags") ? row["tags"] : json::array();
    return tags.empty() || !tags.front().is_string() ? std::string() : tags.front().get<std::string>();
}

MarkupPick MarkupContext::NameOf(const MarkupNameFacts& facts)
{
    const std::string tagToken = std::string(kTokenPrefix) + std::string(facts.Tag);
    const std::vector<MarkupToken> readBack = FindTokens(tagToken);
    const bool tagReads = !facts.Tag.empty() && readBack.size() == 1 && readBack.front().Text == tagToken &&
                          readBack.front().Tag == facts.Tag;
    if (tagReads && facts.SameTag == 1)
        return {tagToken, {}};
    if (!facts.Title.empty() && facts.SameTitle == 1)
        return {std::string(kTokenPrefix) + Quoted(facts.Title), {}};
    if (facts.Title.empty() && facts.Tag.empty())
        return {{}, "Give this mark-up a title or a tag to name it"};

    std::string why;
    if (!facts.Tag.empty())
        why = tagReads ? "Its tag " + std::string(facts.Tag) + " is the first tag of " + std::to_string(facts.SameTag) +
                             " mark-ups"
                       : "Its tag " + std::string(facts.Tag) + " cannot be typed as a token";
    if (!facts.Title.empty())
        why += (why.empty() ? "" : ", and ") + std::to_string(facts.SameTitle) + " mark-ups are titled " +
               std::string(facts.Title);
    const std::string fix = facts.Tag.empty() ? "a tag or another title" : "a tag or a title of its own";
    return {{}, why + ": give this one (entity " + std::to_string(facts.EntityId) + ") " + fix + " to name it"};
}

const json* MarkupContext::Resolve(const MarkupToken& token, const json& rows)
{
    for (const json& row : rows)
    {
        const bool named = token.Tag.empty() ? row.value("title", "") == token.Title : TagOf(row) == token.Tag;
        if (named)
            return &row;
    }
    return nullptr;
}

ECS::EntityHandle MarkupContext::EntityOf(const json& row)
{
    return ECS::EntityHandle(row.value("entityId", 0u));
}

std::string MarkupContext::Block(const json& markup)
{
    const std::string kind = markup.value("kind", "volume");
    const json& shape = markup.contains("shape") ? markup["shape"] : json::object();
    std::string block = "<markup tags=\"" + TagsAttribute(markup) + "\" title=\"" +
                        Attribute(markup.value("title", "")) + "\" status=\"" + Attribute(markup.value("status", "")) +
                        "\" kind=\"" + Attribute(kind) + "\"" + ShapeAttributes(kind, shape) + ">\n";
    // What the shape line, the description, the members and the thread share.
    const std::size_t room = kMaxBlockBytes - kClose.size() - kOverflowLineBytes;

    // The members are the shape's: they keep their room, and the description is cut first.
    std::string members;
    std::size_t membersLeft = 0;
    for (const json& member : shape.value("members", json::array()))
    {
        const std::string line = MemberLine(member) + "\n";
        if (membersLeft > 0 || block.size() + members.size() + line.size() > room)
            ++membersLeft;
        else
            members += line;
    }
    const std::string description = markup.value("description", "");
    const std::size_t used = block.size() + members.size();
    const std::size_t descriptionRoom = room > used + 1 ? room - used - 1 : 0;
    std::size_t descriptionKept = 0;
    const std::string shown = CutBodyText(description, descriptionRoom, false, descriptionKept);
    if (!shown.empty())
        block += shown + "\n";
    block += members;

    const json& entries = markup.contains("entries") ? markup["entries"] : json::array();
    const std::size_t first = entries.size() > kMaxEntries ? entries.size() - kMaxEntries : 0;
    std::size_t entriesLeft = 0;
    for (std::size_t index = first; index < entries.size(); ++index)
    {
        const std::string line = EntryLine(entries[index]) + "\n";
        if (entriesLeft > 0 || block.size() + line.size() > room)
            ++entriesLeft;
        else
            block += line;
    }

    std::string notShown;
    if (descriptionKept < description.size())
        notShown = "the description's last " + std::to_string(description.size() - descriptionKept) + " bytes";
    for (const std::string& part : {Counted(membersLeft, "member", "members"), Counted(entriesLeft, "entry", "entries")})
    {
        if (!part.empty())
            notShown += (notShown.empty() ? "" : ", ") + part;
    }
    if (!notShown.empty())
        block += "[not shown, the 8 KB block cap: " + notShown + "]\n";
    return block + std::string(kClose);
}

MarkupTurnContext MarkupContext::ForPrompt(std::string_view prompt, const Editor::EditorMarkupRequests& requests)
{
    const std::vector<MarkupToken> tokens = FindTokens(prompt);
    if (tokens.empty())
        return {};
    const Editor::MarkupRequestAnswer list = requests.List(json::object());
    MarkupTurnContext context;
    for (std::size_t index = 0; index < tokens.size(); ++index)
    {
        const MarkupToken& token = tokens[index];
        const std::string named = BodyText(token.Text, true);
        MarkupMention& mention = context.Mentions.emplace_back(MarkupMention{token.Text, {}});
        std::string piece;
        if (list.IsRefusal())
        {
            piece = "[mark-up " + named + " not read: " + BodyText(list.Refusal, true) + "]";
            context.Notice = list.Refusal;
        }
        else if (const json* row = Resolve(token, list.Result["markups"]); !row)
        {
            piece = "[unknown mark-up " + named + "]";
            context.Notice = "No mark-up is named " + token.Text + "; the agent is told so";
        }
        else
        {
            mention.Title = row->value("title", "");
            const Editor::MarkupRequestAnswer answer = requests.Query(EntityOf(*row));
            piece = answer.IsRefusal() ? "[mark-up " + named + " not read: " + BodyText(answer.Refusal, true) + "]"
                                       : Block(answer.Result["markup"]);
        }
        // Every piece counts, an unknown token's line included; past the cap the rest are counted.
        if (context.Text.size() + piece.size() + 1 > kMaxTurnBytes - kOverflowLineBytes)
        {
            const std::size_t left = tokens.size() - index;
            context.Text += (context.Text.empty() ? "" : "\n") + std::string("[") + std::to_string(left) +
                            (left == 1 ? " more mark-up" : " more mark-ups") +
                            " not included: the 32 KB per-turn cap]";
            context.Notice = "Mark-ups past 32 KB in one prompt are counted, not included";
            break;
        }
        context.Text += (context.Text.empty() ? "" : "\n") + piece;
    }
    return context;
}
} // namespace GameEngine
