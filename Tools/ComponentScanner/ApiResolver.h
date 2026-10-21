#pragma once

#include "ApiDraft.h"
#include "ApiKindResolver.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace GameEngine::ComponentScanner
{

// Resolves the drafts of every module against the kind table and the types marked in any
// module: kinds, record layouts, defaults, events. A declaration it cannot resolve is refused
// with the file, the line and the fix.
class ApiResolver
{
  public:
    explicit ApiResolver(const std::vector<std::unique_ptr<ModuleDraft>>& modules);

    // The description of every module that marks a declaration, in module order.
    ApiDescription Resolve();

    std::vector<ApiRefusal>& Refusals() { return m_Refusals; }

  private:
    const std::vector<std::unique_ptr<ModuleDraft>>& m_Modules;
    std::vector<ApiNamedType> m_Named;
    std::vector<ApiRefusal> m_Refusals;

    // Resolved records and enums by id, and the ids being resolved (a record that holds itself).
    std::map<std::string, std::optional<ApiRecord>> m_Records;
    std::map<std::string, std::optional<ApiEnum>> m_Enums;
    std::set<std::string> m_Resolving;
    std::map<std::string, std::map<std::string, ApiConstant>> m_ReferencedConstants;

    void Refuse(const std::string& file, int line, std::string problem, std::string fix);
    void CollectNamedTypes();
    std::pair<const ModuleDraft*, const Draft*> FindNamed(const std::string& id) const;
    static ApiSource SourceOf(const HeaderSource& header, int line);
    static const HeaderSource& HeaderOf(const ModuleDraft& module, const Draft& draft);
    ApiLookup LookupFrom(std::string_view scope) const;
    std::optional<ApiEnum> ResolveEnum(const std::string& id);
    std::optional<ApiEnum> ReadEnum(const ModuleDraft& module, const Draft& draft);
    std::optional<ApiRecord> ResolveRecord(const std::string& id);
    std::optional<std::pair<std::uint32_t, std::uint32_t>> LayoutOf(const ApiType& type);
    std::optional<ApiRecord> ReadRecord(const ModuleDraft& module, const Draft& draft);
    std::optional<ApiField> ReadField(const ModuleDraft& module, const Draft& record, const FieldDecl& field);
    bool ReadFieldDefault(const ModuleDraft& module, const Draft& record, const FieldDecl& field, ApiField& result);
    std::optional<ApiDefault> ReadScalarDefault(const ModuleDraft& module, const HeaderSource& header, std::string_view text,
                                                const ApiType& type, DeclarationProblem& outProblem);
    std::optional<ApiFunction> ReadFunction(const ModuleDraft& module, const Draft& draft);
    bool ReadContext(const HeaderSource& header, const Draft& draft, std::vector<ParsedParam>& params,
                     std::optional<std::string>& outContext);
    std::optional<ApiParam> ReadParam(const ModuleDraft& module, const Draft& draft, const ParsedParam& param);
    std::optional<ApiHandle> ReadHandle(const ModuleDraft& module, const Draft& draft);
    std::optional<std::string> CallbackPayload(std::string_view typeText, std::string_view scope);
    void ReadEvents(const ModuleDraft& module, std::vector<ApiEvent>& outEvents);
    std::optional<ApiModule> ResolveModule(const ModuleDraft& module);
};

} // namespace GameEngine::ComponentScanner
