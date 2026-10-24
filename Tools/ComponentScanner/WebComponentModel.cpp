#include "WebComponentModel.h"

#include "FieldKindResolver.h"
#include "Lexical.h"

#include <algorithm>

namespace GameEngine::ComponentScanner
{
namespace
{

// "FovY" -> "fovY", "MeshNameId" -> "meshNameId", "IBLScale" -> "iblScale", "UV" -> "uv":
// the leading capital run is lowered, keeping the capital that starts the next word.
std::string CamelCase(const std::string& name)
{
    std::size_t run = 0;
    while (run < name.size() && Lexical::IsUpper(name[run]))
        ++run;
    if (run == 0)
        return name;
    const std::size_t lower = (run == name.size() || run == 1) ? run : run - 1;
    std::string result = name;
    for (std::size_t i = 0; i < lower; ++i)
        result[i] = Lexical::ToLower(result[i]);
    return result;
}

} // namespace

std::optional<WebComponentModel> WebComponentModel::Build(const std::vector<ReflectedStruct>& reflected,
                                                          const std::map<std::string, std::string>& aliases,
                                                          const EnumTable& enums,
                                                          const std::set<std::string>& knownStructs,
                                                          std::vector<std::string>& errors)
{
    std::vector<const ReflectedStruct*> ordered;
    for (const ReflectedStruct& r : reflected)
        if (!r.EditorOnly)
            ordered.push_back(&r);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const ReflectedStruct* a, const ReflectedStruct* b) { return a->Name < b->Name; });

    WebComponentModel model;
    std::map<std::string, const EnumDecl*> usedEnums;

    for (const ReflectedStruct* r : ordered)
    {
        WebComponent component;
        component.Name = r->Name;
        component.NoAdd = r->NoAdd;
        component.EngineInternal = r->EngineInternal;
        for (const ReflectedField& f : r->Fields)
        {
            if (f.Hidden || f.Transient)
                continue;
            if (f.ArrayExtent && f.ArrayExtent->find(']') != std::string::npos)
            {
                errors.push_back(r->Name + "." + f.Name + " is a multi-dimensional array ([" + *f.ArrayExtent +
                                 "]), which the web types cannot express; declare it with one subscript.");
                continue;
            }
            const ResolvedKind resolved = FieldKindResolver::Resolve(f, aliases, enums);
            if (resolved.Kind == FieldKind::Unknown)
            {
                const WebOmissionReason reason = knownStructs.count(resolved.TypeName) != 0
                                                     ? WebOmissionReason::NoReflectedKind
                                                 : enums.Ambiguous.count(resolved.TypeName) != 0
                                                     ? WebOmissionReason::AmbiguousEnum
                                                     : WebOmissionReason::UnresolvedType;
                model.Omitted.push_back(WebOmission{r->Name, f.Name, f.Type, reason});
                continue;
            }
            if (resolved.Enum)
                usedEnums[resolved.Enum->Name] = resolved.Enum;
            component.Fields.push_back(WebField{f.Name, CamelCase(f.Name), resolved.Kind, resolved.Enum, f.ArrayExtent,
                                                f.ReadOnly, f.Tooltip, f.Label, f.Range});
        }
        model.Components.push_back(std::move(component));
    }

    std::set<std::string> componentNames;
    for (const WebComponent& c : model.Components)
        componentNames.insert(c.Name);
    for (const auto& [enumName, decl] : usedEnums)
        if (componentNames.count(enumName) != 0)
            errors.push_back("the enum " + enumName + " and the component " + enumName +
                             " would share one name in components.d.ts; rename one of them.");

    if (!errors.empty())
        return std::nullopt;
    for (const auto& [enumName, decl] : usedEnums)
        model.Enums.push_back(decl);
    return model;
}

} // namespace GameEngine::ComponentScanner
