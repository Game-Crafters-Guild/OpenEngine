#include "JsonEmitter.h"

#include "JsonWriter.h"

#include <charconv>
#include <cstdint>

namespace GameEngine::ComponentScanner::JsonEmitter
{
namespace
{

// A literal array extent made only of digits.
std::optional<std::int64_t> LiteralCount(const std::string& extent)
{
    if (extent.empty() || extent[0] == '-')
        return std::nullopt;
    std::int64_t value = 0;
    const auto [ptr, ec] = std::from_chars(extent.data(), extent.data() + extent.size(), value);
    if (ec != std::errc{} || ptr != extent.data() + extent.size())
        return std::nullopt;
    return value;
}

void WriteField(JsonWriter& w, const WebField& f)
{
    w.StartObject();
    w.String("name", f.Name);
    w.String("tsName", f.TsName);
    w.String("kind", FieldKindName(f.Kind));
    if (f.Enum)
        w.String("enum", f.Enum->Name);
    if (f.ArrayExtent)
    {
        if (const std::optional<std::int64_t> count = LiteralCount(*f.ArrayExtent))
            w.Number("count", *count);
        else
            w.String("count", *f.ArrayExtent);
    }
    if (f.ReadOnly)
        w.Boolean("readonly", true);
    if (f.Tooltip)
        w.String("tooltip", *f.Tooltip);
    if (f.Range)
    {
        w.Number("min", f.Range->Min);
        if (f.Range->Max)
            w.Number("max", *f.Range->Max);
    }
    w.EndObject();
}

} // namespace

std::string Emit(const WebComponentModel& model)
{
    JsonWriter w;
    w.StartObject();
    w.StartArray("components");
    for (const WebComponent& c : model.Components)
    {
        w.StartObject();
        w.String("name", c.Name);
        w.StartArray("fields");
        for (const WebField& f : c.Fields)
            WriteField(w, f);
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();

    w.StartArray("enums");
    for (const EnumDecl* e : model.Enums)
    {
        w.StartObject();
        w.String("name", e->Name);
        w.StartArray("values");
        for (const EnumMember& m : e->Members)
        {
            w.StartObject();
            w.String("name", m.Name);
            w.Number("value", m.Value);
            w.EndObject();
        }
        w.EndArray();
        w.EndObject();
    }
    w.EndArray();

    w.StartArray("omitted");
    for (const WebOmission& o : model.Omitted)
    {
        w.StartObject();
        w.String("component", o.Component);
        w.String("field", o.Field);
        w.String("type", o.Type);
        w.String("reason", o.Reason == WebOmissionReason::NoReflectedKind ? "noReflectedKind" : "unresolvedType");
        w.EndObject();
    }
    w.EndArray();
    w.EndObject();
    return w.Text + "\n";
}

} // namespace GameEngine::ComponentScanner::JsonEmitter
