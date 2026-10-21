#pragma once

#include "ApiDraft.h"
#include "ApiMarkerLines.h"

#include <filesystem>
#include <map>
#include <memory>
#include <vector>

namespace GameEngine::ComponentScanner
{

// Reads the marked declarations of a module tree into drafts (ApiDraft.h), not yet resolved
// against the kind table, and refuses every marker it cannot attach or read. The markers and
// the attachment rule are in ApiScanner.h.
class ApiDraftReader
{
  public:
    // Reads every module folder under `modulesDir`, in name order, each <Name>ScriptApi.h of a
    // folder as the <Name> script module, and searches every file under each of `searchDirs`
    // (header trees outside the modules) for a stray marker.
    void ReadModules(const std::filesystem::path& modulesDir, const std::vector<std::filesystem::path>& searchDirs);

    std::vector<std::unique_ptr<ModuleDraft>>& Modules() { return m_Modules; }
    std::vector<ApiRefusal>& Refusals() { return m_Refusals; }

  private:
    std::vector<std::unique_ptr<ModuleDraft>> m_Modules;
    std::vector<ApiRefusal> m_Refusals;
    // The include path of the header that declares each script module, by module name.
    std::map<std::string, std::string> m_ModuleIncludes;

    void Refuse(const std::string& file, int line, std::string problem, std::string fix);
    void Refuse(const HeaderSource& header, int line, const DeclarationProblem& problem);
    void ReadModule(const std::filesystem::path& moduleDir);
    void ReadDrafts(ModuleDraft& module, std::size_t headerIndex);
    void RefuseStrayMarkers(const std::filesystem::path& file, const std::string& problem, const std::string& fix);
    void ReadDeclaration(ModuleDraft& module, std::size_t headerIndex, int declLine, const std::vector<int>& run,
                         const std::vector<LineMarkers>& markers, const std::vector<LineCondition>& conditions);
};

} // namespace GameEngine::ComponentScanner
