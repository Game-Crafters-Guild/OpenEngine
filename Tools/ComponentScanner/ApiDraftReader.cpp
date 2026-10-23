#include "ApiDraftReader.h"

#include "CommentStripper.h"
#include "Lexical.h"
#include "ReflectedModel.h"
#include "SourceFiles.h"

#include <algorithm>
#include <iterator>
#include <set>

namespace GameEngine::ComponentScanner
{
namespace
{

using namespace Lexical;
namespace fs = std::filesystem;

constexpr std::string_view kMarkerText = "@ge-script";
constexpr std::string_view kApiHeaderSuffix = "ScriptApi.h";

constexpr std::pair<std::string_view, ApiDelivery> kDeliveries[] = {
    {"PhysicsStep", ApiDelivery::PhysicsStep},
    {"UiEventRouting", ApiDelivery::UiEventRouting},
};

// Every file is searched for a stray marker, whatever its extension.
bool AnyFile(const std::string&)
{
    return true;
}

} // namespace

void ApiDraftReader::ReadModules(const fs::path& modulesDir, const std::vector<fs::path>& searchDirs)
{
    std::vector<fs::path> moduleDirs;
    std::error_code ec;
    for (fs::directory_iterator it(modulesDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_directory(ec))
            moduleDirs.push_back(it->path());
    std::sort(moduleDirs.begin(), moduleDirs.end(),
              [](const fs::path& a, const fs::path& b)
              { return Utf8(a.filename()) < Utf8(b.filename()); });
    for (const fs::path& moduleDir : moduleDirs)
        ReadModule(moduleDir);

    for (const fs::path& searchDir : searchDirs)
        for (const fs::path& file : ListFiles(searchDir, AnyFile))
            RefuseStrayMarkers(file, "a script marker outside a module's script API header",
                               "move the declaration to a module's Include/<Module>/<Module>ScriptApi.h");
}

void ApiDraftReader::RefuseStrayMarkers(const fs::path& file, const std::string& problem, const std::string& fix)
{
    std::string raw;
    if (!ReadText(file, raw) || raw.find(kMarkerText) == std::string::npos)
        return;
    const std::vector<std::string> lines = SplitLines(raw);
    const std::vector<LineMarkers> markers = ApiMarkerLines::MarkersByLine(lines, ApiMarkerLines::ConditionsByLine(lines));
    for (std::size_t line = 1; line < markers.size(); ++line)
        if (markers[line].HasScriptMarker())
            Refuse(Utf8(file), static_cast<int>(line), problem, fix);
}

void ApiDraftReader::Refuse(const std::string& file, int line, std::string problem, std::string fix)
{
    m_Refusals.push_back(ApiRefusal{file, line, std::move(problem), std::move(fix)});
}

void ApiDraftReader::Refuse(const HeaderSource& header, int line, const DeclarationProblem& problem)
{
    Refuse(header.File, line, problem.Problem, problem.Fix);
}

void ApiDraftReader::ReadModule(const fs::path& moduleDir)
{
    const std::string moduleName = Utf8(moduleDir.filename());
    const fs::path includeDir = moduleDir / "Include";
    std::error_code ec;
    if (!fs::is_directory(includeDir, ec))
        return;

    // Every Include/<Module>/*ScriptApi.h, in name order.
    std::vector<fs::path> apiHeaders;
    for (fs::directory_iterator it(includeDir / moduleName, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && EndsWith(Utf8(it->path().filename()), kApiHeaderSuffix))
            apiHeaders.push_back(it->path());
    std::sort(apiHeaders.begin(), apiHeaders.end(),
              [](const fs::path& a, const fs::path& b)
              { return Utf8(a.filename()) < Utf8(b.filename()); });

    std::set<std::string> apiHeaderPaths;
    for (const fs::path& header : apiHeaders)
        apiHeaderPaths.insert(Utf8(FullPath(header)));
    for (const fs::path& file : ListFiles(includeDir, AnyFile))
        if (apiHeaderPaths.count(Utf8(FullPath(file))) == 0)
            RefuseStrayMarkers(file, "a script marker outside a " + moduleName + " script API header",
                               "move the declaration to Include/" + moduleName + "/" + moduleName + std::string(kApiHeaderSuffix) +
                                   " (or another Include/" + moduleName + "/*" + std::string(kApiHeaderSuffix) +
                                   "); only those headers are read for script markers");

    // Each <Name>ScriptApi.h is the <Name> script module: its description, its table and its C# class.
    for (const fs::path& path : apiHeaders)
    {
        std::string raw;
        if (!ReadText(path, raw))
            continue;
        const std::string fileName = Utf8(path.filename());
        const std::string scriptModule = fileName.substr(0, fileName.size() - kApiHeaderSuffix.size());
        const std::string include = moduleName + "/" + fileName;
        if (scriptModule.empty())
        {
            Refuse(Utf8(path), 1, "a script API header is named <Name>ScriptApi.h, and its <Name> names the script module",
                   "rename the header, for example to " + moduleName + std::string(kApiHeaderSuffix));
            continue;
        }
        if (const auto other = m_ModuleIncludes.find(scriptModule); other != m_ModuleIncludes.end())
        {
            Refuse(Utf8(path), 1, "the script module `" + scriptModule + "` is already declared by " + other->second,
                   "rename one of the headers: each <Name>ScriptApi.h is the <Name> script module");
            continue;
        }
        m_ModuleIncludes.emplace(scriptModule, include);

        auto module = std::make_unique<ModuleDraft>();
        module->Name = scriptModule;
        HeaderSource header;
        header.Module = scriptModule;
        header.File = Utf8(path);
        header.Include = include;
        header.Lines = SplitLines(raw);
        StrippedSource stripped = CommentStripper::Strip(raw);
        header.Stripped = std::move(stripped.Text);
        std::vector<SourceWarning> warnings;
        header.Structs = StructParser::Parse(header.Stripped, warnings);
        header.Enums = EnumScanner::Scan(header.Stripped);
        header.Constants = FunctionParser::ScanConstants(header.Stripped);
        module->Headers.push_back(std::move(header));
        ReadDrafts(*module, 0);
        m_Modules.push_back(std::move(module));
    }
}

void ApiDraftReader::ReadDrafts(ModuleDraft& module, std::size_t headerIndex)
{
    const HeaderSource& header = module.Headers[headerIndex];
    const std::vector<std::string>& lines = header.Lines;
    const int lineCount = static_cast<int>(lines.size());
    const std::vector<LineCondition> conditions = ApiMarkerLines::ConditionsByLine(lines);
    const std::vector<LineMarkers> markers = ApiMarkerLines::MarkersByLine(lines, conditions);

    std::set<int> attached;
    std::set<int> declarations;
    for (int line = 1; line <= lineCount; ++line)
    {
        const LineMarkers& here = markers[static_cast<std::size_t>(line)];
        for (const std::string& unknown : here.Unknown)
            Refuse(header.File, line, "`" + unknown + "` is not a script API marker",
                   "use one of the markers: " + ApiMarkerLines::MarkerList());
        if (here.DocMarker)
        {
            attached.insert(line);
            Refuse(header.File, line, "a marker on a `///` line is documentation", "write the marker on its own `//` line");
        }
        if (std::none_of(here.Markers.begin(), here.Markers.end(), IsPrimary))
            continue;

        int declLine = line;
        if (!here.HasCode)
        {
            declLine = line + 1;
            while (declLine <= lineCount && markers[static_cast<std::size_t>(declLine)].CommentOnly)
                ++declLine;
            if (declLine > lineCount || TrimView(lines[static_cast<std::size_t>(declLine - 1)]).empty())
            {
                attached.insert(line);
                Refuse(header.File, line, "the marker has no declaration directly below it: a blank line ends the run",
                       "put the marker on the line directly above the declaration, or delete it");
                continue;
            }
        }
        if (!declarations.insert(declLine).second)
            continue;

        std::vector<int> run{declLine};
        for (int above = declLine - 1; above >= 1 && markers[static_cast<std::size_t>(above)].CommentOnly; --above)
            run.push_back(above);
        std::sort(run.begin(), run.end());
        attached.insert(run.begin(), run.end());
        ReadDeclaration(module, headerIndex, declLine, run, markers, conditions);
    }

    for (int line = 1; line <= lineCount; ++line)
        if (!markers[static_cast<std::size_t>(line)].Markers.empty() && attached.count(line) == 0)
            Refuse(header.File, line, "the marker attaches to no declaration",
                   "put it in the comment lines directly above a declaration marked @ge-script, or delete it");
}

void ApiDraftReader::ReadDeclaration(ModuleDraft& module, std::size_t headerIndex, int declLine, const std::vector<int>& run,
                                     const std::vector<LineMarkers>& markers, const std::vector<LineCondition>& conditions)
{
    const HeaderSource& header = module.Headers[headerIndex];
    std::vector<Marker> found;
    std::optional<std::string> eventPayload;
    std::vector<std::string> docLines;
    for (int line : run)
    {
        const LineMarkers& m = markers[static_cast<std::size_t>(line)];
        found.insert(found.end(), m.Markers.begin(), m.Markers.end());
        if (m.EventPayload)
            eventPayload = m.EventPayload;
        if (m.Doc && line != declLine)
            docLines.push_back(*m.Doc);
    }
    const auto has = [&found](Marker marker)
    { return std::find(found.begin(), found.end(), marker) != found.end(); };

    if (std::count_if(found.begin(), found.end(), IsPrimary) > 1)
    {
        Refuse(header.File, declLine, "the declaration carries more than one of @ge-script, @ge-script-handle and @ge-script-event",
               "keep the one marker that says what it is");
        return;
    }
    if (const LineCondition& condition = conditions[static_cast<std::size_t>(declLine)]; condition.GroupLine != 0)
    {
        Refuse(header.File, declLine,
               "the marked declaration is inside the `" + condition.GroupText + "` group of line " +
                   std::to_string(condition.GroupLine) + ", and the description is the same on every platform",
               "move the declaration out of the group; a platform difference belongs in the function body");
        return;
    }

    const std::string_view text = header.Stripped;
    const std::size_t lineStart = FunctionParser::LineOffset(text, declLine);
    const std::size_t offset = FunctionParser::SkipSpace(text, lineStart);
    const std::size_t lineEnd = text.find('\n', lineStart);
    if (offset >= text.size() || (lineEnd != std::string_view::npos && offset > lineEnd))
    {
        Refuse(header.File, declLine, "the marked line holds no declaration the scanner can read",
               "mark a declaration written out in the header, not one a macro declares");
        return;
    }
    if (FunctionParser::StartsInsideStatement(text, offset))
    {
        Refuse(header.File, declLine, "the marker sits inside a declaration (a template head or a line it continues)",
               "put the marker directly above the declaration's first line; templates cannot be bound");
        return;
    }

    Draft draft;
    draft.HeaderIndex = headerIndex;
    draft.Scope = ScopeName(FunctionParser::ScopeAt(text, offset));
    draft.Line = declLine;
    draft.Doc = ApiMarkerLines::JoinDoc(docLines);
    const bool functionModifiers = has(Marker::Try) || has(Marker::Context) || has(Marker::EngineThread) || has(Marker::AnyThread);
    const std::string word = FunctionParser::FirstWord(text, offset);
    DeclarationProblem problem;

    if (has(Marker::Flags) && !(has(Marker::Script) && word == "enum"))
        return Refuse(header.File, declLine, "@ge-script-flags marks an enum class whose values combine with |",
                      "remove it, or put it on a marked enum class");

    if (has(Marker::Handle) || (has(Marker::Script) && (word == "struct" || word == "enum")))
    {
        if (functionModifiers)
        {
            Refuse(header.File, declLine, "@ge-script-try, @ge-context and the thread markers apply to functions only",
                   "remove them from this declaration");
            return;
        }
    }

    if (has(Marker::Handle))
    {
        std::optional<ParsedAlias> alias = FunctionParser::ParseAlias(text, offset, problem);
        if (!alias)
            return Refuse(header, declLine, problem);
        draft.Kind = DraftKind::Handle;
        draft.Name = alias->Name;
        draft.CppName = Qualify(alias->Scope, alias->Name);
        draft.Alias = std::move(*alias);
        module.Drafts.push_back(std::move(draft));
        return;
    }

    if (has(Marker::Script) && word == "struct")
    {
        const auto it = std::find_if(header.Structs.begin(), header.Structs.end(),
                                     [declLine](const ParsedStruct& s)
                                     { return s.DeclLine == declLine; });
        if (it == header.Structs.end())
            return Refuse(header.File, declLine, "a record must be a struct declared at namespace scope",
                          "declare the struct directly in a namespace");
        draft.Kind = DraftKind::Record;
        draft.StructIndex = static_cast<std::size_t>(it - header.Structs.begin());
        draft.Name = it->Name;
        draft.CppName = QualifiedName(it->Namespace, it->Name);
        draft.Scope = draft.CppName;
        module.Drafts.push_back(std::move(draft));
        return;
    }

    if (has(Marker::Script) && word == "enum")
    {
        const auto it = std::find_if(header.Enums.begin(), header.Enums.end(),
                                     [declLine](const EnumDecl& e)
                                     { return e.DeclLine == declLine; });
        if (it == header.Enums.end())
            return Refuse(header.File, declLine, "the enum cannot be read: an enumerator's value is not an integer literal",
                          "write every value as an integer literal, or leave it implicit");
        draft.Kind = DraftKind::Enum;
        draft.Flags = has(Marker::Flags);
        draft.EnumIndex = static_cast<std::size_t>(it - header.Enums.begin());
        draft.Name = it->Name;
        draft.CppName = Qualify(FunctionParser::ScopeAt(text, offset), it->Name);
        module.Drafts.push_back(std::move(draft));
        return;
    }

    if (word == "class" || word == "union")
        return Refuse(header.File, declLine, "a " + word + " cannot be bound",
                      "mark a struct of plain fields (a record), or free functions over a handle");
    if (word == "using" || word == "typedef")
        return Refuse(header.File, declLine, "an alias is bound only as a handle",
                      "mark a generational handle alias with // @ge-script-handle, or use the kind it names");

    std::optional<ParsedFunction> function = FunctionParser::ParseFunction(text, offset, problem);
    if (!function)
        return Refuse(header, declLine, problem);
    if (function->Scope.InOtherBlock)
        return Refuse(header.File, declLine, "the function is declared inside a function body or a block",
                      "declare it at namespace scope");

    draft.Name = function->Name;
    draft.CppName = Qualify(function->Scope, function->Name);
    draft.Context = has(Marker::Context);

    if (has(Marker::Event))
    {
        if (has(Marker::Try) || has(Marker::EngineThread) || has(Marker::AnyThread))
            return Refuse(header.File, declLine, "an event takes no @ge-script-try or thread marker: its delivery names its thread",
                          "remove the marker");
        if (StartsWith(function->Name, "Subscribe") && function->Name.size() > 9)
        {
            draft.Kind = DraftKind::Subscribe;
            const auto delivery = std::find_if(std::begin(kDeliveries), std::end(kDeliveries),
                                               [&](const auto& d)
                                               { return eventPayload && d.first == *eventPayload; });
            if (delivery == std::end(kDeliveries))
                return Refuse(header.File, declLine,
                              eventPayload ? "unknown event delivery `" + *eventPayload + "`" : "the event names no delivery",
                              "write `// @ge-script-event PhysicsStep` or `// @ge-script-event UiEventRouting`");
            draft.Delivery = delivery->second;
        }
        else if (StartsWith(function->Name, "Unsubscribe") && function->Name.size() > 11)
        {
            draft.Kind = DraftKind::Unsubscribe;
            if (eventPayload)
                return Refuse(header.File, declLine, "the delivery is written on the Subscribe function only",
                              "write `// @ge-script-event` on Unsubscribe" + function->Name.substr(11));
        }
        else
            return Refuse(header.File, declLine, "@ge-script-event marks a Subscribe<Name> and Unsubscribe<Name> pair",
                          "name the functions Subscribe<Name> and Unsubscribe<Name>");
    }
    else
    {
        draft.Kind = DraftKind::Function;
        draft.Try = has(Marker::Try);
        if (has(Marker::EngineThread) && has(Marker::AnyThread))
            return Refuse(header.File, declLine, "the function carries two thread markers", "keep one of @ge-engine-thread and @ge-any-thread");
        draft.Thread = has(Marker::EngineThread) ? ApiThread::Engine : has(Marker::AnyThread) ? ApiThread::Any
                                                                                              : ApiThread::Main;
    }
    draft.Function = std::move(*function);
    module.Drafts.push_back(std::move(draft));
}

} // namespace GameEngine::ComponentScanner
