#include "ComponentScanner.h"

#include "ApiJsonEmitter.h"
#include "ApiKindResolver.h"
#include "ApiScanner.h"
#include "BindingRules.h"
#include "CSharpBindingEmitter.h"
#include "CommentStripper.h"
#include "ComponentMirror.h"
#include "ComponentMirrorCSharpEmitter.h"
#include "ComponentMirrorLayoutEmitter.h"
#include "ComponentMirrorResolver.h"
#include "DtsEmitter.h"
#include "EnumScanner.h"
#include "FieldKindResolver.h"
#include "IncludeGraph.h"
#include "JsonEmitter.h"
#include "Lexical.h"
#include "MarkerAttachment.h"
#include "MarkerScanner.h"
#include "NativeBindingEmitter.h"
#include "ReflectedModel.h"
#include "ReflectionEmitter.h"
#include "ScopeLookup.h"
#include "SourceFiles.h"
#include "StructParser.h"
#include "TypeAliasScanner.h"
#include "WebComponentModel.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>

namespace GameEngine::ComponentScanner
{
namespace
{

namespace fs = std::filesystem;

constexpr std::string_view kComponentNamespace = "GameEngine::Components";

// CRTP base for per-entity systems (ECS::EntitySystem<Self>). Its derivation of SystemBase is
// invisible to the scanner (it only sees the direct base), so it is matched by name alongside
// the configured --detect-system-base.
constexpr std::string_view kEntitySystemBase = "EntitySystem";

struct SkippedStruct
{
    std::string Name;
    std::string Header;
    std::string Reason;
};

bool EqualsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (Lexical::ToLower(a[i]) != Lexical::ToLower(b[i]))
            return false;
    return true;
}

// Header extensions the scanner reflects from: any of these can be #include'd by the
// generated registration TU. The file watcher (NativeScriptManager) watches .h/.hpp, so
// .hpp components must be detected too.
bool IsHeaderExtension(const std::string& ext)
{
    return EqualsIgnoreCase(ext, ".h") || EqualsIgnoreCase(ext, ".hpp") || EqualsIgnoreCase(ext, ".hxx") ||
           EqualsIgnoreCase(ext, ".hh");
}

// Non-header C++ translation units. A macro-free component here can't be reflected (the
// generated TU #includes by header path), so it is diagnosed, not registered.
bool IsNonHeaderCppSource(const std::string& ext)
{
    return EqualsIgnoreCase(ext, ".cpp") || EqualsIgnoreCase(ext, ".cc") || EqualsIgnoreCase(ext, ".cxx");
}

// Locate "Engine/Include" by walking up from the scanned directory so include paths can be
// made relative to it ("Components/Rendering/Camera.h"). For a module dir
// (Modules/HeightFog/Include/Components) that check fails; the fallback returns the parent of
// the nearest "Components" dir, which is that module's Include root.
fs::path FindIncludeRoot(const fs::path& scanDir)
{
    const fs::path full = FullPath(scanDir);
    for (fs::path dir = full; !dir.empty(); dir = dir.parent_path())
    {
        const fs::path parent = dir.parent_path();
        if (EqualsIgnoreCase(Utf8(dir.filename()), "Include") && !parent.empty() && parent != dir &&
            EqualsIgnoreCase(Utf8(parent.filename()), "Engine"))
            return dir;
        if (parent == dir)
            break;
    }
    for (fs::path dir = full; !dir.empty(); dir = dir.parent_path())
    {
        const fs::path parent = dir.parent_path();
        if (EqualsIgnoreCase(Utf8(dir.filename()), "Components") && !parent.empty() && parent != dir)
            return parent;
        if (parent == dir)
            break;
    }
    return full;
}

std::string ToIncludePath(const fs::path& header, const fs::path& includeRoot)
{
    return Utf8(FullPath(header).lexically_relative(includeRoot));
}

// Two enum declarations are the same if their enumerators match in order, name, and value.
bool SameMembers(const EnumDecl& a, const EnumDecl& b)
{
    if (a.Members.size() != b.Members.size())
        return false;
    for (std::size_t i = 0; i < a.Members.size(); ++i)
        if (a.Members[i].Name != b.Members[i].Name || a.Members[i].Value != b.Members[i].Value)
            return false;
    return true;
}

// True if every std:: token in the type is a known POD fixed-width / byte / size alias; a
// type with "std::" but no identifier after it has no such token and does not qualify.
bool IsPodStdAlias(const std::string& type)
{
    std::size_t tokens = 0;
    std::size_t pos = type.find("std::");
    while (pos != std::string::npos)
    {
        std::size_t end = pos + 5;
        if (end < type.size() && Lexical::IsIdentifierStart(type[end]))
        {
            const std::size_t start = end;
            while (end < type.size() && Lexical::IsIdentifierChar(type[end]))
                ++end;
            const std::string_view name = std::string_view(type).substr(start, end - start);
            const bool pod = name == "int8_t" || name == "int16_t" || name == "int32_t" || name == "int64_t" ||
                             name == "uint8_t" || name == "uint16_t" || name == "uint32_t" || name == "uint64_t" ||
                             name == "size_t" || name == "ptrdiff_t" || name == "intptr_t" || name == "uintptr_t" ||
                             name == "byte";
            if (!pod)
                return false;
            ++tokens;
        }
        pos = type.find("std::", end);
    }
    return tokens > 0;
}

bool IsAscii(std::string_view text)
{
    return std::all_of(text.begin(), text.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

// True for "AssetRef<AssetType::X>", qualified or not: the one templated field type the
// registry classifies (FieldTypeTraits in Components/AssetRef.h). Its argument holds no
// template of its own.
bool IsAssetRefType(std::string_view type)
{
    const std::size_t open = type.find('<');
    if (open == std::string_view::npos || type.back() != '>' || type.find('<', open + 1) != std::string_view::npos)
        return false;
    return ApiKindResolver::NamesQualified(Lexical::TrimView(type.substr(0, open)), "GameEngine::Components::AssetRef");
}

// Decide whether a struct should be skipped: nullopt to reflect it, or the reason to skip it.
// No field-count cap: the emitted cap-free BEGIN/FIELD/END form has no argument limit. A struct
// with no fields is reflected only as a tag (`tag`, the struct carries // @ge-tag).
std::optional<std::string> EvaluateSkip(const ParsedStruct& s, bool tag)
{
    // Only ASCII names are reflected: the generated registration code (which MSVC reads in the
    // system code page unless built with /utf-8), the scene files' type keys and the web types
    // are not verified to carry any other.
    if (!IsAscii(s.Name))
        return "its name is not ASCII; rename it with ASCII letters, digits and underscores";
    if (s.HasVirtual)
        return "has a virtual member (non-POD)";
    if (s.UnexpandedMacro)
        return "body uses unexpanded field-injection macro '" + *s.UnexpandedMacro +
               "' (fields not visible without the preprocessor)";
    if (s.Fields.empty() && !tag)
        return "0 extractable fields";

    for (const FieldDecl& f : s.Fields)
    {
        const std::string& t = f.Type;
        const std::string trimmed = Lexical::Trim(t);
        if (t.find('*') != std::string::npos)
            return "field '" + f.Name + "' type '" + trimmed + "' contains a pointer (*)";
        if (t.find('&') != std::string::npos)
            return "field '" + f.Name + "' type '" + trimmed + "' contains a reference (&)";
        if (t.find('<') != std::string::npos && !IsAssetRefType(trimmed))
            return "field '" + f.Name + "' type '" + trimmed + "' is templated (<...>)";
        // std:: is conservative against non-POD containers (std::vector, std::string,
        // std::unique_ptr, ...). Plain fixed-width integer/byte aliases (std::int32_t,
        // std::uint64_t, std::size_t, std::byte) are POD and reflect fine, so allow them.
        if (t.find("std::") != std::string::npos && !IsPodStdAlias(t))
            return "field '" + f.Name + "' type '" + trimmed + "' uses std:: (likely non-POD container)";
    }
    return std::nullopt;
}

// The text of a value marker, or nullopt when the marker is absent or its text is blank.
std::optional<std::string> NonBlank(std::optional<std::string> text)
{
    if (text && Lexical::TrimView(*text).empty())
        return std::nullopt;
    return text;
}

// Attach each enumerator's "// @ge-label Text": on the enumerator's own line or in the comment
// run above it, the rules a field's markers follow.
void ReadEnumLabels(EnumDecl& decl, const std::vector<std::string>& lines, const std::map<int, std::string>& labels)
{
    for (EnumMember& member : decl.Members)
        member.Label = NonBlank(MarkerAttachment::FieldValueMarkerAttached(lines, member.Line, member.Line, labels));
}

// Read one field's markers: "// @ge-hidden", "// @ge-readonly", a field-level
// "// [DoNotSerialize]" (FieldFlags::Transient), "// @ge-tooltip Text", "// @ge-label Text" and
// "// @ge-range Min [Max]". MarkerAttachment decides which line belongs to which field. A hidden
// field has no inspector row, so its label is ignored with a warning.
ReflectedField ReadField(const fs::path& header, const std::vector<std::string>& lines, const FieldDecl& field,
                         const MarkerLines& markers, std::ostream& err)
{
    ReflectedField result;
    result.Name = field.Name;
    result.Type = field.Type;
    result.ArrayExtent = field.ArrayExtent;
    result.Hidden = MarkerAttachment::FieldMarkerAttached(lines, field.DeclLine, field.EndLine, markers.Hidden);
    result.ReadOnly = MarkerAttachment::FieldMarkerAttached(lines, field.DeclLine, field.EndLine, markers.ReadOnly);
    result.Transient = MarkerAttachment::FieldMarkerAttached(lines, field.DeclLine, field.EndLine, markers.DoNotSerialize);
    result.Tooltip = NonBlank(MarkerAttachment::FieldValueMarkerAttached(lines, field.DeclLine, field.EndLine, markers.Tooltips));
    result.Label = NonBlank(MarkerAttachment::FieldValueMarkerAttached(lines, field.DeclLine, field.EndLine, markers.Labels));
    if (result.Label && result.Hidden)
    {
        err << "warning: " << Utf8(header) << ":" << field.DeclLine << ": field '" << field.Name
            << "' is @ge-hidden and has no inspector row, so its @ge-label is ignored. Fix: remove one of the two markers.\n";
        result.Label.reset();
    }
    result.Range = MarkerAttachment::FieldValueMarkerAttached(lines, field.DeclLine, field.EndLine, markers.Ranges);
    result.Line = field.DeclLine;
    return result;
}

// Order includes to mirror the hand-written ComponentReflection.cpp: the canonical leading
// order for the well-known headers, then the remaining ones alphabetically.
std::vector<std::string> OrderIncludes(const std::set<std::string>& headers)
{
    constexpr std::string_view kPreferred[] = {
        "Components/Transform.h",
        "Components/Hierarchy.h",
        "Components/Name.h",
        "Components/Animation/AnimatorRef.h",
        "Components/Rendering/Camera.h",
        "Components/Rendering/Light.h",
    };

    std::vector<std::string> ordered;
    for (std::string_view p : kPreferred)
        if (headers.count(std::string(p)) != 0)
            ordered.emplace_back(p);

    for (const std::string& h : headers)
        if (std::find(ordered.begin(), ordered.end(), h) == ordered.end())
            ordered.push_back(h);

    return ordered;
}

const char* OmissionAdvice(WebOmissionReason reason)
{
    switch (reason)
    {
    case WebOmissionReason::NoReflectedKind:
        return "a struct with no reflected kind. Give it a FieldTypeTraits specialization the scanner knows, or mark "
               "the field // @ge-hidden if a page never reads it.";
    case WebOmissionReason::AmbiguousEnum:
        return "two different enums share this name, so the scanner cannot give the field a name table (the warning "
               "above names their headers). Rename one of the enums.";
    default:
        return "the type is not declared in the scanned component headers, so the scanner cannot resolve its kind. "
               "An enum must be declared in a scanned header or in a header one includes from an include root "
               "(--enum-include-root); a platform-sized integer (long, "
               "size_t) should be a fixed-width type; a struct needs a FieldTypeTraits specialization.";
    }
}

void ReportWarnings(const fs::path& header, const std::vector<SourceWarning>& warnings, std::ostream& err)
{
    for (const SourceWarning& warning : warnings)
        err << "warning: " << Utf8(header) << ":" << warning.Line << ": " << warning.Message << "\n";
}

// A macro the scanner cannot expand that declares a `: <baseName>` struct: the component it
// declares is never registered, so the macro is named with the fix.
void ReportComponentMacros(const fs::path& header, const std::vector<MacroDefinition>& macros, std::string_view baseName,
                           std::ostream& err)
{
    for (const MacroDefinition& macro : macros)
    {
        if (InheritingStructNames(macro.Body, baseName).empty())
            continue;
        err << "warning: " << Utf8(header) << ":" << macro.Line << ": macro '" << macro.Name
            << "' declares a struct inheriting " << baseName
            << ", and the scanner does not expand macros, so it registers no component the macro declares; declare "
               "the struct directly in the header, or register it with GE_REGISTER_COMPONENT.\n";
    }
}

bool WriteOutput(const fs::path& path, const std::string& text, std::ostream& out, std::ostream& err)
{
    const fs::path full = FullPath(path);
    std::error_code ec;
    if (!full.parent_path().empty())
        fs::create_directories(full.parent_path(), ec);
    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        err << "Failed to write " << Utf8(full) << "\n";
        return false;
    }
    file << text;
    out << "Wrote " << Utf8(full) << "\n";
    return true;
}

// Writes each (path relative to `dir`, text) into `dir` and deletes every other file under it
// whose name ends in `suffix`, so a module or a component header that is gone leaves no
// generated file behind.
bool WriteGeneratedFiles(const fs::path& dir, const std::map<std::string, std::string>& files, std::string_view suffix,
                         std::ostream& out, std::ostream& err)
{
    std::error_code ec;
    fs::create_directories(FullPath(dir), ec);
    for (const auto& [name, text] : files)
        if (name.ends_with(suffix) && !WriteOutput(dir / PathFromUtf8(name), text, out, err))
            return false;
    std::vector<fs::path> stale;
    for (fs::recursive_directory_iterator it(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        const std::string name = Utf8(it->path().lexically_relative(dir));
        if (it->is_regular_file(ec) && name.ends_with(suffix) && files.count(name) == 0)
            stale.push_back(it->path());
    }
    for (const fs::path& file : stale)
    {
        std::error_code removeError;
        if (!fs::remove(file, removeError))
        {
            err << "Failed to remove " << Utf8(file) << "\n";
            return false;
        }
        out << "Removed " << Utf8(file) << "\n";
    }
    return true;
}

// What a component scan collects from its roots: the reflected components or systems, the
// skipped structs, the headers they come from, and the enums, aliases and struct names the web
// types resolve field types with.
struct ComponentCollection
{
    // Every header of a component scan, in scan order; ReflectedStruct::Source indexes it.
    std::vector<ComponentHeader> Headers;
    std::vector<ReflectedStruct> Reflected;
    std::vector<ReflectedSystem> Systems;
    std::vector<SkippedStruct> Skipped;
    std::set<std::string> HeadersWithComponents;
    std::set<std::string> HeadersWithSystems;
    // De-dupe structs reflected from more than one root (by fully-qualified name).
    std::set<std::string> SeenQualified;
    std::vector<fs::path> IncludeRoots;
    EnumTable Enums;
    // `using` aliases across all scanned headers, for the web emitters' kind resolution; an
    // alias declared twice with different targets is dropped.
    std::map<std::string, std::string> Aliases;
    // Simple names of every struct declared in the scanned headers, in any namespace: a field
    // of such a type with no reflected kind is Unknown in the registry too (WebOmissionReason).
    std::set<std::string> KnownStructs;
    std::set<std::string> AmbiguousAliases;
};

// Scans every root's headers into `result`: components (engine mode, or inheriting
// `detectBase`) or systems (`systemBase`). False, with the error written, when a root is not a
// directory.
bool CollectComponents(const std::vector<fs::path>& inDirs, const std::optional<std::string>& detectBase,
                       const std::optional<std::string>& systemBase, ComponentCollection& result, std::ostream& err)
{
    for (const fs::path& inDir : inDirs)
    {
        std::error_code ec;
        if (!fs::is_directory(inDir, ec))
        {
            err << "Input directory does not exist: " << Utf8(inDir) << "\n";
            return false;
        }

        // Engine mode resolves includes relative to Engine/Include (yielding
        // "Components/Transform.h"). User (--detect-base) mode has only the scanned SourceDir
        // on the generated DLL's include path, so emit includes relative to it ("Foo.h",
        // "Combat/Foo.h") — FindIncludeRoot's Engine/Components heuristic would otherwise pick
        // the parent of a SourceDir named "Components".
        const fs::path includeRoot = detectBase ? FullPath(inDir) : FindIncludeRoot(inDir);
        result.IncludeRoots.push_back(includeRoot);

        for (const fs::path& header : ListFiles(inDir, IsHeaderExtension))
        {
            std::string raw;
            if (!ReadText(header, raw))
            {
                err << "Failed to read " << Utf8(header) << ": cannot open the file\n";
                continue;
            }

            StrippedSource stripped = CommentStripper::Strip(raw);
            const std::string& cleaned = stripped.Text;
            const std::vector<ParsedStruct> structs = StructParser::Parse(cleaned, stripped.Warnings);
            ReportWarnings(header, stripped.Warnings, err);
            if (detectBase)
                ReportComponentMacros(header, stripped.Macros, *detectBase, err);
            for (const ParsedStruct& parsed : structs)
                result.KnownStructs.insert(parsed.Name);

            // Marker comments ("// @ge-no-add", "// @ge-editor-only", "// @ge-readonly",
            // "// @ge-hidden", "// @ge-tooltip ...", "// @ge-label ...") are captured from the RAW
            // text because the comment stripper has already removed them from `cleaned`.
            std::vector<std::string> lines = SplitLines(raw);
            const MarkerLines markers = MarkerScanner::Scan(lines, err);

            // Collect enum declarations for name-based serialization of enum fields (component
            // modes only — a system isn't reflected). Last unique definition wins; a conflicting
            // redefinition of the same simple name is flagged ambiguous.
            if (!systemBase)
            {
                for (EnumDecl& ed : EnumScanner::Scan(cleaned))
                {
                    ReadEnumLabels(ed, lines, markers.Labels);
                    const auto existing = result.Enums.ByName.find(ed.Name);
                    if (existing != result.Enums.ByName.end())
                    {
                        if (!SameMembers(existing->second, ed))
                            result.Enums.Ambiguous.insert(ed.Name);
                    }
                    else
                    {
                        result.Enums.ByName.emplace(ed.Name, std::move(ed));
                    }
                }

                for (auto& [name, target] : TypeAliasScanner::Scan(cleaned))
                {
                    const auto existing = result.Aliases.find(name);
                    if (existing != result.Aliases.end() && existing->second != target)
                        result.AmbiguousAliases.insert(name);
                    result.Aliases[name] = target;
                }
            }

            const std::string includePath = ToIncludePath(header, includeRoot);
            bool fileHadReflected = false;

            for (const ParsedStruct& s : structs)
            {
                // System mode (--detect-system-base SystemBase): collect structs inheriting
                // `: ... SystemBase` (global systems) OR `: ... EntitySystem<Self>` (per-entity
                // systems — the CRTP base derives SystemBase, but the scanner only sees the
                // direct base name, so match it explicitly). Both register identically: the
                // generated UserSystemAdapter<T> ticks T::OnUpdate, which for a per-entity system
                // is EntitySystem's inherited OnUpdate.
                if (systemBase)
                {
                    if (!InheritsBase(s.Bases, *systemBase) && !InheritsBase(s.Bases, kEntitySystemBase))
                        continue;
                    if (!result.SeenQualified.insert(QualifiedName(s.Namespace, s.Name)).second)
                        continue; // already seen from an earlier root
                    result.Systems.push_back(ReflectedSystem{s.Name, s.Namespace, includePath});
                    result.HeadersWithSystems.insert(includePath);
                    continue;
                }

                // Engine mode (default): structs declared directly in GameEngine::Components.
                // User mode (--detect-base ComponentBase): structs inheriting
                // `: ... ComponentBase`, in any namespace.
                const bool isComponent = detectBase ? InheritsBase(s.Bases, *detectBase) : s.Namespace == kComponentNamespace;
                if (!isComponent)
                    continue;

                if (std::optional<std::string> skipReason =
                        EvaluateSkip(s, MarkerAttachment::StructMarkerAttached(lines, s.DeclLine, markers.Tag)))
                {
                    result.Skipped.push_back(SkippedStruct{s.Name, includePath, std::move(*skipReason)});
                    continue;
                }

                if (!result.SeenQualified.insert(QualifiedName(s.Namespace, s.Name)).second)
                    continue; // already reflected from an earlier root

                ReflectedStruct r;
                r.Name = s.Name;
                r.Namespace = s.Namespace;
                r.Header = includePath;
                r.NoAdd = MarkerAttachment::StructMarkerAttached(lines, s.DeclLine, markers.NoAdd);
                r.DoNotSerialize = MarkerAttachment::StructMarkerAttached(lines, s.DeclLine, markers.DoNotSerialize);
                r.EditorOnly = MarkerAttachment::StructMarkerAttached(lines, s.DeclLine, markers.EditorOnly);
                const std::optional<int> internalLine = MarkerAttachment::StructMarkerLine(lines, s.DeclLine, markers.EngineInternal);
                const std::optional<int> readOnlyLine = MarkerAttachment::StructMarkerLine(lines, s.DeclLine, markers.ReadOnly);
                r.EngineInternal = internalLine.has_value();
                r.ReadOnly = readOnlyLine.has_value();
                if (const std::optional<int> writerLine = internalLine ? internalLine : readOnlyLine)
                    r.Writer = markers.Writers.at(*writerLine);
                r.Source = result.Headers.size();
                r.Line = s.DeclLine;
                for (const FieldDecl& f : s.Fields)
                    r.Fields.push_back(ReadField(header, lines, f, markers, err));
                result.Reflected.push_back(std::move(r));
                fileHadReflected = true;
            }

            if (fileHadReflected)
                result.HeadersWithComponents.insert(includePath);
            if (!systemBase)
                result.Headers.push_back(ComponentHeader{Utf8(header), includePath, std::move(lines), std::move(stripped.Text)});
        }
    }
    return true;
}

// True when the scan resolved no kind for `field` and no scanned struct or ambiguous enum
// explains it: an enum it names may be declared in a header the component header includes.
bool NeedsIncludedEnum(const ComponentCollection& collection, const ReflectedField& field)
{
    const ResolvedKind resolved = FieldKindResolver::Resolve(field, collection.Aliases, collection.Enums);
    return resolved.Kind == FieldKind::Unknown && collection.KnownStructs.count(resolved.TypeName) == 0 &&
           collection.Enums.Ambiguous.count(resolved.TypeName) == 0;
}

// An enum a field names, found in a header a component header includes.
struct IncludedEnum
{
    ScopedEnumDecl Scoped;
    const IncludedHeader* Header = nullptr;
};

// "Power/Modes.h:2 (GameEngine::Power::Mode)".
std::string DescribeIncludedEnum(const IncludedEnum& found)
{
    return found.Header->IncludePath + ":" + std::to_string(found.Scoped.Decl.DeclLine) + " (" + found.Scoped.CppName + ")";
}

// Adds to the enum table each enum a reflected field names that no scanned header declares: the
// name is looked up from the component's scope (ScopeLookup), among the enums of the headers its
// header includes (IncludeGraph: beside the including header, then under the scan's include roots
// and `enumIncludeRoots`). Only those enums are taken. The generated table holds one enum per
// simple name, so two different enums the fields name under one simple name are left out, with a
// warning naming both headers. Returns the headers that declared a taken enum.
std::vector<fs::path> ResolveIncludedEnums(ComponentCollection& collection, const std::vector<fs::path>& enumIncludeRoots,
                                           std::ostream& err)
{
    std::vector<fs::path> includeRoots = collection.IncludeRoots;
    includeRoots.insert(includeRoots.end(), enumIncludeRoots.begin(), enumIncludeRoots.end());
    IncludeGraph graph(std::move(includeRoots));
    std::set<fs::path> componentFiles;
    for (const ComponentHeader& header : collection.Headers)
        componentFiles.insert(FullPath(PathFromUtf8(header.File)));

    std::map<const IncludedHeader*, std::vector<ScopedEnumDecl>> enumsByHeader;
    std::map<std::string, std::vector<IncludedEnum>> taken;
    for (const ReflectedStruct& r : collection.Reflected)
    {
        std::vector<const ReflectedField*> needing;
        for (const ReflectedField& f : r.Fields)
            if (NeedsIncludedEnum(collection, f))
                needing.push_back(&f);
        if (needing.empty())
            continue;

        const ComponentHeader& header = collection.Headers[r.Source];
        std::vector<IncludedEnum> candidates;
        std::vector<std::string> names;
        for (const IncludedHeader* included : graph.Closure(PathFromUtf8(header.File), header.Lines))
        {
            if (componentFiles.count(included->File) != 0)
                continue;
            auto [it, inserted] = enumsByHeader.try_emplace(included);
            if (inserted)
            {
                it->second = EnumScanner::ScanScoped(included->Stripped);
                const MarkerLines markers = MarkerScanner::Scan(included->Lines, err);
                for (ScopedEnumDecl& scoped : it->second)
                    ReadEnumLabels(scoped.Decl, included->Lines, markers.Labels);
            }
            for (const ScopedEnumDecl& scoped : it->second)
            {
                candidates.push_back(IncludedEnum{scoped, included});
                names.push_back(scoped.CppName);
            }
        }

        const std::string scope = QualifiedName(r.Namespace, r.Name);
        for (const ReflectedField* f : needing)
        {
            const std::string written = ScopeLookup::BareType(f->Type);
            const std::size_t index = ScopeLookup::LookUp(written, scope, names);
            if (index == ScopeLookup::kAmbiguous)
            {
                std::string headers;
                for (std::size_t i = 0; i < names.size(); ++i)
                    if (ApiKindResolver::NamesQualified(written, names[i]))
                        headers += (headers.empty() ? "" : " and ") + DescribeIncludedEnum(candidates[i]);
                err << "warning: " << r.Name << "." << f->Name << " names the enum `" << written
                    << "`, which more than one included header declares: " << headers
                    << ". Fix: qualify the name as C++ finds it from the component.\n";
                continue;
            }
            if (index >= candidates.size())
                continue;
            const IncludedEnum& chosen = candidates[index];
            std::vector<IncludedEnum>& sameName = taken[chosen.Scoped.Decl.Name];
            const bool known = std::any_of(sameName.begin(), sameName.end(),
                                           [&chosen](const IncludedEnum& e) { return e.Scoped.CppName == chosen.Scoped.CppName; });
            if (!known)
                sameName.push_back(chosen);
        }
    }

    std::vector<fs::path> sources;
    for (auto& [name, found] : taken)
    {
        if (found.size() > 1)
        {
            std::string headers;
            for (const IncludedEnum& e : found)
                headers += (headers.empty() ? "" : " and ") + DescribeIncludedEnum(e);
            err << "warning: the enum name `" << name << "` stands for two enums the components' fields name: " << headers
                << ". The generated name table holds one enum per name, so their fields save numbers. Fix: rename one "
                   "of the enums.\n";
            collection.Enums.Ambiguous.insert(name);
            continue;
        }
        sources.push_back(found.front().Header->File);
        collection.Enums.ByName.emplace(name, std::move(found.front().Scoped.Decl));
    }
    return sources;
}

// A path as a make-style dependency file writes it: absolute, forward slashes, a space escaped.
std::string DependencyPath(const fs::path& file)
{
    std::string text;
    for (char c : Utf8(FullPath(file)))
    {
        if (c == ' ')
            text += '\\';
        text += c;
    }
    return text;
}

// A make-style dependency file for the build: `output` depends on every header in `headers`, so
// an edit to a header that declares a reflected field's enum re-runs the scan.
bool WriteDependencyFile(const fs::path& path, const fs::path& output, const std::vector<fs::path>& headers,
                         std::ostream& out, std::ostream& err)
{
    std::string text = DependencyPath(output) + ":";
    for (const fs::path& header : headers)
        text += " " + DependencyPath(header);
    return WriteOutput(path, text + "\n", out, err);
}

// Where an --emit-api run writes the bindings generated from the description: the native
// thunks and tables (<Module>.script.gen.h and .cpp) and the C# (<Module>.g.cs). Either may be
// absent.
struct BindingOutputs
{
    std::optional<fs::path> NativeDir;
    std::optional<fs::path> CSharpDir;
};

// The --emit-api mode: scan the module tree's script API headers, report every refusal, and
// write the description only when there is none: one <Module>.json per module into `apiDir`,
// every other .json there deleted, so a removed facade leaves no file behind. With binding
// outputs, a declaration the generators cannot emit is a refusal too, and the bindings are
// written beside the description the same way.
// With component roots, the scan also mirrors their components in C# (ComponentMirror.h): it writes
// component-classes.txt beside the description, the C# mirrors under the C# binding's Components
// directory and the layout pins beside the native bindings, and a component the rules refuse fails
// the scan like a refused declaration.
int RunApiScan(const fs::path& modulesDir, const std::vector<fs::path>& searchDirs, const std::vector<fs::path>& componentDirs,
               const fs::path& apiDir, const BindingOutputs& bindings, bool dryRun, std::ostream& out, std::ostream& err)
{
    std::error_code ec;
    if (!fs::is_directory(modulesDir, ec))
    {
        err << "Module directory does not exist: " << Utf8(modulesDir) << "\n";
        return 2;
    }
    for (const fs::path& searchDir : searchDirs)
    {
        if (!fs::is_directory(searchDir, ec))
        {
            err << "Search directory does not exist: " << Utf8(searchDir) << "\n";
            return 2;
        }
    }
    ApiScanResult result = ApiScanner::Scan(modulesDir, searchDirs);
    if ((bindings.NativeDir || bindings.CSharpDir) && result.Refusals.empty())
        result.Refusals = BindingRules::Unsupported(result.Description);
    ComponentMirrors mirrors;
    if (!componentDirs.empty())
    {
        ComponentCollection components;
        if (!CollectComponents(componentDirs, std::nullopt, std::nullopt, components, err))
            return 2;
        for (const std::string& name : components.AmbiguousAliases)
            components.Aliases.erase(name);
        mirrors = ComponentMirrorResolver::Resolve(components.Headers, components.Reflected, components.Aliases, modulesDir,
                                                   result.Description, result.Refusals);
    }
    for (const ApiRefusal& refusal : result.Refusals)
        err << "error: " << refusal.File << ":" << refusal.Line << ": " << refusal.Problem << ". Fix: " << refusal.Fix << "\n";

    std::size_t functions = 0;
    std::size_t records = 0;
    std::size_t enums = 0;
    std::size_t handles = 0;
    std::size_t events = 0;
    for (const ApiModule& module : result.Description.Modules)
    {
        functions += module.Functions.size();
        records += module.Records.size();
        enums += module.Enums.size();
        handles += module.Handles.size();
        events += module.Events.size();
    }
    out << "ComponentScanner.api modules=" << result.Description.Modules.size() << " functions=" << functions
        << " records=" << records << " enums=" << enums << " handles=" << handles << " events=" << events
        << " refused=" << result.Refusals.size() << "\n";
    if (!componentDirs.empty())
    {
        std::map<ComponentClass, std::size_t> classes;
        for (const MirrorStruct& mirror : mirrors.Structs)
            ++classes[mirror.Class];
        out << "ComponentScanner.components reflected=" << mirrors.Structs.size() << " mirrored=" << classes[ComponentClass::GameData]
            << " readonly=" << classes[ComponentClass::EngineOwned] << " element=" << classes[ComponentClass::Element]
            << " internal=" << classes[ComponentClass::EngineInternal] << " editor=" << classes[ComponentClass::EditorOnly]
            << " enums=" << mirrors.Enums.size() << "\n";
    }
    if (!result.Refusals.empty())
    {
        err << "The script API description cannot be generated; nothing was written.\n";
        return 1;
    }
    if (dryRun)
        return 0;
    std::map<std::string, std::string> description;
    std::map<std::string, std::string> native;
    std::map<std::string, std::string> csharp;
    const BindingRules::Index index = BindingRules::BuildIndex(result.Description);
    for (const ApiModule& module : result.Description.Modules)
    {
        description[module.Name + ".json"] = ApiJsonEmitter::EmitModule(module);
        if (bindings.NativeDir)
        {
            const std::string stem = BindingRules::NativeFileStem(module);
            native[stem + ".h"] = NativeBindingEmitter::EmitHeader(module, index);
            native[stem + ".cpp"] = NativeBindingEmitter::EmitSource(module, index);
        }
        if (bindings.CSharpDir)
            csharp[BindingRules::CSharpFile(module)] = CSharpBindingEmitter::Emit(module, index);
    }
    if (!componentDirs.empty() && bindings.CSharpDir)
        for (auto& [name, text] : ComponentMirrorCSharpEmitter::Emit(mirrors))
            csharp[std::string(kComponentCSharpDir) + "/" + name] = std::move(text);
    if (!WriteGeneratedFiles(apiDir, description, ".json", out, err))
        return 1;
    if (!componentDirs.empty() && !WriteOutput(apiDir / kComponentClassesFile, ComponentClassesText(mirrors), out, err))
        return 1;
    if (bindings.NativeDir && (!WriteGeneratedFiles(*bindings.NativeDir, native, ".script.gen.h", out, err) ||
                               !WriteGeneratedFiles(*bindings.NativeDir, native, ".script.gen.cpp", out, err)))
        return 1;
    if (!componentDirs.empty() && bindings.NativeDir &&
        !WriteOutput(*bindings.NativeDir / kComponentLayoutFile, ComponentMirrorLayoutEmitter::Emit(mirrors), out, err))
        return 1;
    if (bindings.CSharpDir && !WriteGeneratedFiles(*bindings.CSharpDir, csharp, ".g.cs", out, err))
        return 1;
    return 0;
}

} // namespace

bool InheritsBase(std::string_view bases, std::string_view baseName)
{
    if (Lexical::TrimView(bases).empty())
        return false;
    std::size_t start = 0;
    while (start <= bases.size())
    {
        std::size_t comma = bases.find(',', start);
        if (comma == std::string_view::npos)
            comma = bases.size();
        const std::string_view entry = bases.substr(start, comma - start);
        start = comma + 1;

        std::string type;
        for (const std::string& tok : Lexical::SplitWhitespace(entry))
        {
            if (tok == "public" || tok == "private" || tok == "protected" || tok == "virtual")
                continue;
            type = tok; // specifiers stripped; the trailing token is the base type
        }
        if (type.empty())
            continue;
        if (const std::size_t lt = type.find('<'); lt != std::string::npos)
            type = type.substr(0, lt);
        if (Lexical::LastScopeSegment(type) == baseName)
            return true;
    }
    return false;
}

std::vector<std::string> InheritingStructNames(std::string_view cleanedSource, std::string_view baseName)
{
    std::vector<std::string> names;
    std::vector<SourceWarning> warnings;
    for (const ParsedStruct& s : StructParser::Parse(cleanedSource, warnings))
        if (InheritsBase(s.Bases, baseName))
            names.push_back(s.Name);
    return names;
}

int Run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err)
{
    std::vector<fs::path> inDirs;
    std::optional<fs::path> outFile;
    std::optional<fs::path> dtsFile;
    std::optional<fs::path> jsonFile;
    std::optional<fs::path> apiModulesDir;
    std::vector<fs::path> apiSearchDirs;
    std::vector<fs::path> enumIncludeRoots;
    std::optional<fs::path> depFile;
    std::vector<fs::path> apiComponentDirs;
    std::optional<fs::path> apiDir;
    BindingOutputs bindingOutputs;
    bool dryRun = false;
    // When set, a struct is a component if it inherits a base with this simple name
    // ("ComponentBase" for `: ECS::ComponentBase`), regardless of namespace. Otherwise
    // detection falls back to the engine rule: structs in GameEngine::Components.
    std::optional<std::string> detectBase;
    // When set ("SystemBase" for `: ECS::SystemBase`), the scanner runs in SYSTEM mode: it
    // detects system structs by that base name and emits a UserSystemRegistration TU instead
    // of component reflection. Mutually exclusive with the component scan (a separate
    // invocation per kind). No field extraction / POD-skip applies — a system isn't a POD.
    std::optional<std::string> systemBase;

    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string& arg = args[i];
        const bool takesValue = arg == "--in" || arg == "--out" || arg == "--emit-dts" || arg == "--emit-json" ||
                                arg == "--detect-base" || arg == "--detect-system-base" || arg == "--api-modules" ||
                                arg == "--api-search" || arg == "--api-components" || arg == "--emit-api" ||
                                arg == "--emit-native" || arg == "--emit-csharp" || arg == "--enum-include-root" ||
                                arg == "--depfile";
        if (takesValue)
        {
            if (i + 1 >= args.size())
            {
                err << "Missing value for " << arg << "\n";
                return 2;
            }
            const std::string& value = args[++i];
            if (arg == "--in")
                inDirs.push_back(PathFromUtf8(value));
            else if (arg == "--out")
                outFile = PathFromUtf8(value);
            else if (arg == "--emit-dts")
                dtsFile = PathFromUtf8(value);
            else if (arg == "--emit-json")
                jsonFile = PathFromUtf8(value);
            else if (arg == "--detect-base")
                detectBase = value;
            else if (arg == "--api-modules")
                apiModulesDir = PathFromUtf8(value);
            else if (arg == "--api-search")
                apiSearchDirs.push_back(PathFromUtf8(value));
            else if (arg == "--enum-include-root")
                enumIncludeRoots.push_back(PathFromUtf8(value));
            else if (arg == "--depfile")
                depFile = PathFromUtf8(value);
            else if (arg == "--api-components")
                apiComponentDirs.push_back(PathFromUtf8(value));
            else if (arg == "--emit-api")
                apiDir = PathFromUtf8(value);
            else if (arg == "--emit-native")
                bindingOutputs.NativeDir = PathFromUtf8(value);
            else if (arg == "--emit-csharp")
                bindingOutputs.CSharpDir = PathFromUtf8(value);
            else
                systemBase = value;
            continue;
        }
        if (arg == "--dry-run")
        {
            dryRun = true;
            continue;
        }
        err << "Unknown argument: " << arg << "\n";
        return 2;
    }

    if (apiDir || apiModulesDir || !apiSearchDirs.empty() || !apiComponentDirs.empty() || bindingOutputs.NativeDir ||
        bindingOutputs.CSharpDir)
    {
        if (!apiDir || !apiModulesDir || !inDirs.empty() || outFile || dtsFile || jsonFile || detectBase || systemBase)
        {
            err << "--emit-api <dir> takes --api-modules <dir> (and --api-search <dir>, --api-components <dir>, --emit-native "
                   "<dir>, --emit-csharp <dir>) and runs alone: the script API scan is its own invocation.\n";
            return 2;
        }
        return RunApiScan(*apiModulesDir, apiSearchDirs, apiComponentDirs, *apiDir, bindingOutputs, dryRun, out, err);
    }

    if (inDirs.empty())
        inDirs.emplace_back("Engine/Include/Components");

    const bool emitWeb = dtsFile || jsonFile;
    if (emitWeb && systemBase)
    {
        err << "--emit-dts and --emit-json describe components; they do not apply with --detect-system-base.\n";
        return 2;
    }

    if (depFile && !outFile)
    {
        err << "--depfile names the dependencies of --out; pass --out with it.\n";
        return 2;
    }

    ComponentCollection collection;
    if (!CollectComponents(inDirs, detectBase, systemBase, collection, err))
        return 2;
    const std::vector<fs::path> enumSources =
        systemBase ? std::vector<fs::path>{} : ResolveIncludedEnums(collection, enumIncludeRoots, err);
    std::vector<ReflectedStruct>& reflected = collection.Reflected;
    std::vector<ReflectedSystem>& systems = collection.Systems;
    std::vector<SkippedStruct>& skipped = collection.Skipped;
    const std::set<std::string>& headersWithComponents = collection.HeadersWithComponents;
    const std::set<std::string>& headersWithSystems = collection.HeadersWithSystems;
    const std::vector<fs::path>& includeRoots = collection.IncludeRoots;
    const EnumTable& enums = collection.Enums;
    std::map<std::string, std::string>& aliases = collection.Aliases;
    const std::set<std::string>& knownStructs = collection.KnownStructs;
    const std::set<std::string>& ambiguousAliases = collection.AmbiguousAliases;

    // User-mode diagnostic: a macro-free `: <detectBase>` component authored in a .cpp (not a
    // header) is never registered — the scanner only reflects headers (the generated TU
    // #includes them by path) and there is no macro to self-register it. Warn so the silent
    // no-op becomes actionable ("my component never shows up").
    int sourceFileWarnings = 0;
    if (detectBase)
    {
        for (const fs::path& inDir : inDirs)
        {
            for (const fs::path& src : ListFiles(inDir, IsNonHeaderCppSource))
            {
                std::string raw;
                if (!ReadText(src, raw))
                    continue;
                for (const std::string& name : InheritingStructNames(CommentStripper::Strip(raw).Text, *detectBase))
                {
                    err << "warning: '" << name << "' inherits " << *detectBase << " in " << Utf8(src)
                        << " but the scanner only registers components declared in a header (.h/.hpp); move it to a "
                           "header or use GE_REGISTER_COMPONENT.\n";
                    ++sourceFileWarnings;
                }
            }
        }
    }

    std::string generated;

    if (systemBase)
    {
        // System mode: emit one RegisterUserSystem line per detected ECS::SystemBase struct.
        generated = ReflectionEmitter::EmitSystemRegistrationCpp(
            systems, std::vector<std::string>(headersWithSystems.begin(), headersWithSystems.end()));

        out << "=== ComponentScanner (system mode) ===\n";
        for (const fs::path& inDir : inDirs)
            out << "Scanned root: " << Utf8(FullPath(inDir)) << "\n";
        out << "Systems     : " << systems.size() << "\n";
        out << "\n";
        out << "--- SYSTEMS ---\n";
        std::vector<const ReflectedSystem*> sortedSystems;
        for (const ReflectedSystem& s : systems)
            sortedSystems.push_back(&s);
        std::stable_sort(sortedSystems.begin(), sortedSystems.end(),
                         [](const ReflectedSystem* a, const ReflectedSystem* b) { return a->Name < b->Name; });
        for (const ReflectedSystem* s : sortedSystems)
            out << "  " << QualifiedName(s->Namespace, s->Name) << " [" << s->Header << "]\n";
        out << "\n";
        out << "ComponentScanner.summary systems=" << systems.size() << "\n";
    }
    else
    {
        generated = ReflectionEmitter::EmitGeneratedCpp(reflected, OrderIncludes(headersWithComponents),
                                                        detectBase.has_value(), enums);

        std::size_t enumFieldCount = 0;
        for (const ReflectedStruct& r : reflected)
            for (const ReflectedField& f : r.Fields)
                if (enums.Match(f.Type))
                    ++enumFieldCount;

        out << "=== ComponentScanner ===\n";
        for (std::size_t i = 0; i < inDirs.size(); ++i)
            out << "Scanned root: " << Utf8(FullPath(inDirs[i]))
                << "  (include root: " << Utf8(includeRoots[i]) << ")\n";
        out << "Reflected   : " << reflected.size() << "\n";
        out << "Skipped     : " << skipped.size() << "\n";
        out << "Enum fields : " << enumFieldCount << " (name-serialized)\n";
        out << "\n";

        out << "--- REFLECTED ---\n";
        std::vector<const ReflectedStruct*> sortedReflected;
        for (const ReflectedStruct& r : reflected)
            sortedReflected.push_back(&r);
        std::stable_sort(sortedReflected.begin(), sortedReflected.end(),
                         [](const ReflectedStruct* a, const ReflectedStruct* b) { return a->Name < b->Name; });
        for (const ReflectedStruct* r : sortedReflected)
        {
            std::string flagged;
            for (const ReflectedField& f : r->Fields)
            {
                if (!ReflectionEmitter::FieldFlagExpr(f))
                    continue;
                if (!flagged.empty())
                    flagged += ", ";
                flagged += f.Name;
            }
            out << "  " << r->Name << " (" << r->Fields.size() << ")" << (r->NoAdd ? " NO_ADD" : "")
                << (r->EditorOnly ? " EDITOR_ONLY" : "") << (flagged.empty() ? "" : "  [flags: " + flagged + "]") << "\n";
        }
        out << "\n";

        out << "--- SKIPPED ---\n";
        std::vector<const SkippedStruct*> sortedSkipped;
        for (const SkippedStruct& sk : skipped)
            sortedSkipped.push_back(&sk);
        std::stable_sort(sortedSkipped.begin(), sortedSkipped.end(),
                         [](const SkippedStruct* a, const SkippedStruct* b) { return a->Name < b->Name; });
        for (const SkippedStruct* sk : sortedSkipped)
            out << "  " << sk->Name << " [" << sk->Header << "] -- " << sk->Reason << "\n";
        out << "\n";

        // Stable, locale-independent summary the native-build pipeline parses to surface scan
        // warnings (0 reflected / skipped structs / .cpp components) on the console.
        out << "ComponentScanner.summary reflected=" << reflected.size() << " skipped=" << skipped.size()
            << " sourcewarnings=" << sourceFileWarnings << "\n";
    }

    std::string dts;
    std::string json;
    if (emitWeb)
    {
        for (const std::string& name : ambiguousAliases)
            aliases.erase(name);
        std::vector<std::string> errors;
        const std::optional<WebComponentModel> model = WebComponentModel::Build(reflected, aliases, enums, knownStructs, errors);
        if (!model)
        {
            for (const std::string& error : errors)
                err << "error: " << error << "\n";
            err << "The web types cannot be generated; nothing was written.\n";
            return 1;
        }
        for (const WebOmission& o : model->Omitted)
            err << "warning: " << o.Component << "." << o.Field << " (type '" << o.Type
                << "') is left out of the web types: " << OmissionAdvice(o.Reason) << "\n";
        dts = DtsEmitter::Emit(*model);
        json = JsonEmitter::Emit(*model);
        std::size_t fieldCount = 0;
        for (const WebComponent& c : model->Components)
            fieldCount += c.Fields.size();
        out << "ComponentScanner.web components=" << model->Components.size() << " fields=" << fieldCount
            << " enums=" << model->Enums.size() << " omitted=" << model->Omitted.size() << "\n";
    }

    if (dryRun)
    {
        out << "--- GENERATED (dry-run, not written) ---\n";
        out << generated << "\n";
        return 0;
    }

    if (!outFile)
    {
        err << "No --out specified and not --dry-run; nothing to write.\n";
        return 2;
    }

    if (!WriteOutput(*outFile, generated, out, err))
        return 1;
    if (dtsFile && !WriteOutput(*dtsFile, dts, out, err))
        return 1;
    if (jsonFile && !WriteOutput(*jsonFile, json, out, err))
        return 1;
    if (depFile && !WriteDependencyFile(*depFile, *outFile, enumSources, out, err))
        return 1;
    return 0;
}

} // namespace GameEngine::ComponentScanner
