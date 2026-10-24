#pragma once

#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace GameEngine::ComponentScanner
{

// Inspector bounds harvested from "@ge-range". Max is nullopt for an open maximum.
struct FieldRange
{
    double Min = 0.0;
    std::optional<double> Max;
};

// 1-based source-line locations of opt-out / field-policy marker comments, captured from
// RAW (un-stripped) source. The comment stripper removes comments before the structural
// parse, so these markers — which live inside "//" line comments — would otherwise be
// invisible. They are harvested up front and associated with structs (NoAdd, EditorOnly)
// or individual fields (ReadOnly / Hidden / Tooltip / Label / Range) and enumerators (Label)
// afterwards, by line number.
struct MarkerLines
{
    // Struct-level: "// @ge-no-add" -> register the component but keep it out of the
    // editor's Add Component menu.
    std::set<int> NoAdd;

    // Struct-level: "// @ge-editor-only" -> the component saves and loads in the editor, and
    // a game export strips its lines from every staged scene (the entity stays).
    std::set<int> EditorOnly;

    // Struct-level: "// @ge-engine-internal" -> engine runtime state that is no game API: the
    // component has no C# mirror (ComponentMirror.h).
    std::set<int> EngineInternal;

    // Struct-level: "// @ge-tag" -> a struct with no fields is a component (a tag) and is
    // registered; a struct with no fields and no marker is not a component.
    std::set<int> Tag;

    // The text after "@ge-readonly" or "@ge-engine-internal" on its line: on a struct, the system
    // that writes the component, which a refused script write names.
    std::map<int, std::string> Writers;

    // Field-level: "// @ge-readonly" / "// @ge-hidden" -> FieldFlags::ReadOnly / Hidden. On a
    // struct, "// @ge-readonly" makes the component engine-owned: its C# mirror is read-only.
    std::set<int> ReadOnly;
    std::set<int> Hidden;

    // Field-level: "// @ge-tooltip Text" or "// @ge-tooltip \"Text\"" -> FieldInfo::Tooltip.
    std::map<int, std::string> Tooltips;

    // Field- or enumerator-level: "// @ge-label Text" or "// @ge-label \"Text\"" -> the words the
    // editor shows for the field's row (FieldInfo::Label) or for the enumerator (EnumNameValue::Label).
    std::map<int, std::string> Labels;

    // Field-level: "// @ge-range Min Max" or "// @ge-range Min" (open maximum) -> inspector
    // bounds via GE_REFLECT_FIELD_RANGE. Only the float row consumes them.
    std::map<int, FieldRange> Ranges;

    // Struct- OR field-level: "// [DoNotSerialize]" (aliases: [NonSerializable] /
    // [DontSerialize] and the "@" forms) -> reflected for the inspector but excluded from
    // scene serialization. A struct marker excludes the whole component; a field marker
    // excludes that one field.
    std::set<int> DoNotSerialize;
};

namespace MarkerScanner
{
// Scan the (1-based-indexable) raw source lines for marker comments. A marker counts only
// inside a "//" line comment and is detected regardless of surrounding text ("// @ge-readonly
// runtime handle, not user-editable"). A single line may carry more than one distinct
// marker; each is recorded independently. A malformed @ge-range is reported on `warnings`.
MarkerLines Scan(const std::vector<std::string>& lines, std::ostream& warnings);
}

} // namespace GameEngine::ComponentScanner
