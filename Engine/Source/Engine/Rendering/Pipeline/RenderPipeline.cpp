#include "Core/CpuProfiler.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"

#include "Engine/Rendering/Pipeline/ExpressionEvaluator.h"
#include "Engine/Rendering/Pipeline/Nodes/WorldRenderNode.h"
#include "Engine/Rendering/Pipeline/PipelineEnumTables.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowWorldPass.h"
#include "Logger/Logger.h"
#include "Rendering/Core/CullingStrategy.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Types/FlatMap.h"
#include "Types/NearestName.h"
#include "Types/StringUtils.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace GameEngine::Engine::Renderer { using namespace ::GameEngine::Rendering; }

namespace GameEngine::Engine::Renderer::Pipeline
{
using namespace ::GameEngine::Rendering;

namespace
{
static uint64_t Fnv1a64(const void* data, size_t size)
{
    constexpr uint64_t kOffset = 1469598103934665603ull;
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t h = kOffset;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        h ^= (uint64_t)p[i];
        h *= kPrime;
    }
    return h;
}

static uint64_t HashString(const std::string& s)
{
    return Fnv1a64(s.data(), s.size());
}

static bool JsonTryGetString(const nlohmann::json& j, const char* key, std::string& out)
{
    if (!j.is_object() || !key)
        return false;
    auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return false;
    out = it->get<std::string>();
    return true;
}

static bool JsonTryGetBool(const nlohmann::json& j, const char* key, bool& out)
{
    if (!j.is_object() || !key)
        return false;
    auto it = j.find(key);
    if (it == j.end() || !it->is_boolean())
        return false;
    out = it->get<bool>();
    return true;
}

int32_t ParsePassPhase(const std::string& str)
{
    static const std::unordered_map<std::string, int32_t> kMap = {
        {"EarlySetup",  Rendering::PassPhase::kEarlySetup},
        {"SkyRender",   Rendering::PassPhase::kSkyRender},
        {"WorldRender", Rendering::PassPhase::kWorldRender},
        {"PostProcess", Rendering::PassPhase::kPostProcess},
        {"Overlay",     Rendering::PassPhase::kOverlay},
        {"UI",          Rendering::PassPhase::kUI},
        {"Finalize",    Rendering::PassPhase::kFinalize},
        {"Present",     Rendering::PassPhase::kPresent},
    };
    auto it = kMap.find(str);
    if (it != kMap.end())
        return it->second;
    char* end = nullptr;
    long val = std::strtol(str.c_str(), &end, 10);
    if (end != str.c_str() && *end == '\0')
        return static_cast<int32_t>(val);
    Logger::Log::Warning("[RenderPipeline] Unknown phase '{}', defaulting to WorldRender", str);
    return Rendering::PassPhase::kDefault;
}

// The render graph pool name of a blueprint resource: "Pipeline.<pipeline>.View<id>.<name>" for a
// per-view resource, "Pipeline.<pipeline>.<name>" for a frame-scope one.
std::string PoolResourceName(const std::string& pipelineName, bool perView, Rendering::ViewId viewId,
                             std::string_view name)
{
    std::string poolName = "Pipeline." + pipelineName + ".";
    if (perView)
        poolName.append("View").append(std::to_string(static_cast<uint32_t>(viewId))).append(".");
    poolName.append(name);
    return poolName;
}

} // namespace

bool RenderPipelineNodeRegistry::Register(const std::string& type, RenderPipelineNodeFactory factory, bool perView,
                                          RenderPipelineNodeResourceFields resourceFields, bool feedsDepthPrepass)
{
    if (type.empty() || !factory)
        return false;
    const ECS::ModuleRegistrationStamp& active = ECS::GetActiveRegistrationModule();
    if (auto it = m_Types.find(type); it != m_Types.end())
    {
        // Reload replacement (C12): the same module's newer load re-registers
        // the type — swap the factory so instantiation dispatches into the
        // newest mapped image. Anything else keeps first-wins.
        if (!active.Supersedes(it->second.Module))
            return false;
        it->second.factory = std::move(factory);
        it->second.perView = perView;
        it->second.ResourceFields = std::move(resourceFields);
        it->second.FeedsDepthPrepass = feedsDepthPrepass;
        it->second.Module = active;
        Logger::Log::Info("[RenderPipeline] Node type '{}' re-registered by module '{}' generation {} "
                          "(reload) — factory replaced in place",
                          type, active.ModuleId, active.Generation);
        return true;
    }
    RenderPipelineNodeTypeInfo info{};
    info.type = type;
    info.factory = std::move(factory);
    info.perView = perView;
    info.ResourceFields = std::move(resourceFields);
    info.FeedsDepthPrepass = feedsDepthPrepass;
    info.Module = active;
    m_Types[type] = std::move(info);
    return true;
}

std::size_t RenderPipelineNodeRegistry::RetireSupersededModuleNodes(std::string_view moduleId,
                                                                    std::uint64_t currentGeneration)
{
    if (moduleId.empty())
        return 0;
    std::size_t retired = 0;
    for (auto it = m_Types.begin(); it != m_Types.end();)
    {
        const ECS::ModuleRegistrationStamp& stamp = it->second.Module;
        if (stamp.ModuleId == moduleId && stamp.Generation < currentGeneration)
        {
            Logger::Log::Info("[RenderPipeline] Node type '{}' no longer registered by module '{}' "
                              "(generation {} -> {}) — retired",
                              it->first, stamp.ModuleId, stamp.Generation, currentGeneration);
            it = m_Types.erase(it);
            ++retired;
        }
        else
        {
            ++it;
        }
    }
    return retired;
}

std::size_t RenderPipelineNodeRegistry::PurgeModuleNodes(std::string_view moduleId, std::uint64_t generation)
{
    if (moduleId.empty())
        return 0;
    std::size_t purged = 0;
    for (auto it = m_Types.begin(); it != m_Types.end();)
    {
        if (it->second.Module.Matches(moduleId, generation))
        {
            Logger::Log::Warning("[RenderPipeline] Node type '{}' purged: its module '{}' (generation {}) "
                                 "failed to load and is being unmapped",
                                 it->first, moduleId, generation);
            it = m_Types.erase(it);
            ++purged;
        }
        else
        {
            ++it;
        }
    }
    return purged;
}

std::size_t RenderPipelineNodeRegistry::CountSupersededModuleNodes(std::string_view moduleId,
                                                                   std::uint64_t currentGeneration) const
{
    if (moduleId.empty())
        return 0;
    std::size_t stale = 0;
    for (const auto& [type, info] : m_Types)
    {
        if (info.Module.ModuleId == moduleId && info.Module.Generation < currentGeneration)
        {
            Logger::Log::Warning("[RenderPipeline] Node type '{}' still owned by superseded generation {} "
                                 "of module '{}' (current {})",
                                 type, info.Module.Generation, moduleId, currentGeneration);
            ++stale;
        }
    }
    return stale;
}

bool RenderPipelineNodeRegistry::HasModuleNodes(std::string_view moduleId) const
{
    if (moduleId.empty())
        return false;
    for (const auto& [type, info] : m_Types)
    {
        if (info.Module.ModuleId == moduleId)
            return true;
    }
    return false;
}

const RenderPipelineNodeTypeInfo* RenderPipelineNodeRegistry::Find(const std::string& type) const
{
    auto it = m_Types.find(type);
    return it != m_Types.end() ? &it->second : nullptr;
}

std::vector<std::string> RenderPipelineNodeRegistry::GetRegisteredTypes() const
{
    std::vector<std::string> out;
    out.reserve(m_Types.size());
    for (const auto& kv : m_Types)
        out.push_back(kv.first);
    std::sort(out.begin(), out.end());
    return out;
}

namespace
{
// ── Blueprint validation (S2.3) ─────────────────────────────────────────────
// Resolves every resource ref against the four View.* builtins + declared
// resources + the names passes publish (each node type's reference and publish
// keys come from its registration, RenderPipelineNodeResourceFields;
// colorResolveTarget is checked on every pass), parses each resource blob's enums via
// the shared tables, reflects each pass's shaderpkg, checks skipWhen keys against
// the PostProcessSettings field registry, and checks inputGates entries against the
// pass's inputs and the settings gates. Errors are rejected wholesale
// at the FrameOrchestrator enforcement point (last-good pipeline kept);
// warnings apply-but-log.

// The passes that feed the camera depth prepass (RenderPipelineNodeTypeInfo::FeedsDepthPrepass) are
// declared ahead of the first DepthPrepass, in their authored order, wherever the blueprint lists them:
// the prepass declares its reads of their buffers when it is declared, and a feeder declared after it
// would have the graph order the prepass's draw before the compute that fills those buffers. A feeder
// authored below the prepass is moved and reported as a Warning (DepthPrepassFeederMoved) that names it, the
// prepass, and the place to list it: between the pass authored just above the prepass and the prepass.
void DeclareDepthPrepassFeedersFirst(RenderPipelineBlueprint& bp, const RenderPipelineNodeRegistry& registry)
{
    const auto prepass = std::find_if(bp.passes.begin(), bp.passes.end(),
                                      [](const RenderPipelineBlueprint::Pass& pass) { return pass.type == "DepthPrepass"; });
    if (prepass == bp.passes.end())
        return;
    const std::string prepassId = prepass->id;
    const std::string place = prepass == bp.passes.begin()
                                  ? "first, above '" + prepassId + "'"
                                  : "between '" + std::prev(prepass)->id + "' and '" + prepassId + "'";
    const auto feedsPrepass = [&registry](const RenderPipelineBlueprint::Pass& pass)
    {
        const RenderPipelineNodeTypeInfo* info = registry.Find(pass.type);
        return info && info->FeedsDepthPrepass;
    };
    const auto movedEnd = std::stable_partition(prepass, bp.passes.end(), feedsPrepass);
    for (auto moved = prepass; moved != movedEnd; ++moved)
    {
        PipelineIssue issue{};
        issue.severity = PipelineIssueSeverity::Warning;
        issue.code = PipelineIssueCode::DepthPrepassFeederMoved;
        issue.nodeId = moved->id;
        issue.message = "pass '" + moved->id + "' (" + moved->type + ") feeds the depth prepass but is listed after DepthPrepass '" +
                        prepassId + "', so the compiler declares it above '" + prepassId + "'. List it " + place +
                        " to clear this warning";
        bp.issues.push_back(std::move(issue));
    }
}

bool RequiresShaderPkg(const std::string& type)
{
    return type == "FullscreenShader" || type == "TemporalFullscreenShader" ||
           type == "ComputeShader";
}

const RenderPipelineNodeResourceFields& ResourceFieldsForType(const RenderPipelineNodeRegistry& registry,
                                                              const std::string& type)
{
    // Compile drops passes of unregistered types before validation, so every
    // validated pass has an entry; the empty fallback keeps the lookup total.
    static const RenderPipelineNodeResourceFields kNoFields{};
    const RenderPipelineNodeTypeInfo* info = registry.Find(type);
    return info ? info->ResourceFields : kNoFields;
}

// The view resources the spine seeds every frame: a pass left out never leaves them unwritten.
// Other view names (View.GTAO, View.HZB, ...) are published by passes and count like any resource.
bool IsSpineSeeded(const std::string& name)
{
    return name == Names::View::Color || name == Names::View::Depth || name == Names::View::Resolve ||
           name == Names::View::DepthResolved;
}

// Every string value in a pass's JSON other than its id and type: for a pass whose type is not
// registered, every resource it may write.
void CollectNamedResources(const nlohmann::json& value, bool topLevel, std::unordered_set<std::string>& out)
{
    if (value.is_string())
    {
        out.insert(value.get<std::string>());
        return;
    }
    if (!value.is_object() && !value.is_array())
        return;
    for (auto it = value.begin(); it != value.end(); ++it)
    {
        if (topLevel && value.is_object() && (it.key() == "id" || it.key() == "type"))
            continue;
        CollectNamedResources(*it, false, out);
    }
}

// Calls `write` with each resource a registered pass writes: its publish keys (or their defaults),
// its static publishes and the scene color it resolves into.
template <typename WriteFn>
void ForEachWrite(const RenderPipelineNodeResourceFields& fields, const nlohmann::json& pass, WriteFn&& write)
{
    for (const auto& [key, defaultName] : fields.PublishKeys)
    {
        std::string value;
        if (JsonTryGetString(pass, key.c_str(), value) && !value.empty())
            write(value);
        else if (!defaultName.empty())
            write(defaultName);
    }
    for (const auto& name : fields.StaticPublishNames)
        write(name);
    if (std::string target; JsonTryGetString(pass, "colorResolveTarget", target) && !target.empty())
        write(target);
}

void CollectWrites(const RenderPipelineNodeResourceFields& fields, const nlohmann::json& pass,
                   std::unordered_set<std::string>& out)
{
    ForEachWrite(fields, pass, [&out](const std::string& name) { out.insert(name); });
}

// The resources a registered pass reads: its reference keys that it does not publish, and the
// values of its reference maps.
std::vector<std::string> CollectReads(const RenderPipelineNodeResourceFields& fields, const nlohmann::json& pass)
{
    std::vector<std::string> reads;
    for (const auto& key : fields.RefKeys)
    {
        const bool published = std::any_of(fields.PublishKeys.begin(), fields.PublishKeys.end(),
                                           [&key](const auto& publish) { return publish.first == key; });
        std::string value;
        if (!published && JsonTryGetString(pass, key.c_str(), value) && !value.empty())
            reads.push_back(value);
    }
    for (const auto& key : fields.RefMapKeys)
    {
        const auto it = pass.find(key);
        if (it == pass.end() || !it->is_object())
            continue;
        for (auto entry = it->begin(); entry != it->end(); ++entry)
            if (entry.value().is_string())
                reads.push_back(entry.value().get<std::string>());
    }
    return reads;
}

// The resources each waiting pass may write, mapped to that pass. A pass of an unregistered type
// may write any resource its JSON names (its fields are unknown, so a name it only reads counts
// too: the rule errs toward waiting); a registered pass writes what its fields publish. The view
// resources the spine seeds are never left unwritten.
std::unordered_map<std::string, std::string> WaitingPassWrites(
    const std::vector<std::pair<std::string, nlohmann::json>>& waiting, const RenderPipelineNodeRegistry& registry)
{
    std::unordered_map<std::string, std::string> writer;
    for (const auto& [id, json] : waiting)
    {
        std::unordered_set<std::string> names;
        const auto type = json.value("type", std::string());
        if (registry.Find(type))
            CollectWrites(ResourceFieldsForType(registry, type), json, names);
        else
            CollectNamedResources(json, true, names);
        for (const auto& name : names)
            if (!IsSpineSeeded(name))
                writer.emplace(name, id);
    }
    return writer;
}

// The resources the kept passes before `end`, in pass order, write this frame.
std::unordered_set<std::string> KeptWritesBefore(const RenderPipelineBlueprint& bp,
                                                 const std::vector<nlohmann::json>& keptJson,
                                                 const RenderPipelineNodeRegistry& registry, size_t end)
{
    std::unordered_set<std::string> writes;
    for (size_t i = 0; i < end; ++i)
        CollectWrites(ResourceFieldsForType(registry, bp.passes[i].type), keptJson[i], writes);
    return writes;
}

// While the project's scripts build, a pass that reads a resource a waiting pass may write, and no
// kept pass before it writes, waits too: drawn, it would read a texture or buffer nothing wrote this
// frame. Moves such passes out of the blueprint, transitively, as PendingScriptReader issues naming
// the pass they wait on, and adds a PendingScriptOutput issue when the pipeline's output itself
// comes from a waiting pass. `waiting` holds each waiting pass's id and JSON.
void LeaveOutPassesThatReadWaitingOnes(RenderPipelineBlueprint& bp, const RenderPipelineNodeRegistry& registry,
                                       std::vector<std::pair<std::string, nlohmann::json>> waiting)
{
    std::vector<nlohmann::json> keptJson;
    keptJson.reserve(bp.passes.size());
    for (const auto& pass : bp.passes)
        keptJson.push_back(nlohmann::json::parse(pass.passJson, nullptr, false));

    for (bool moved = true; moved;)
    {
        moved = false;
        const auto writer = WaitingPassWrites(waiting, registry);
        for (size_t i = 0; i < bp.passes.size() && !moved; ++i)
        {
            const auto keptBefore = KeptWritesBefore(bp, keptJson, registry, i);
            for (const auto& read : CollectReads(ResourceFieldsForType(registry, bp.passes[i].type), keptJson[i]))
            {
                const auto source = writer.find(read);
                if (source == writer.end() || keptBefore.count(read) != 0)
                    continue;
                PipelineIssue iss{};
                iss.severity = PipelineIssueSeverity::Info;
                iss.code = PipelineIssueCode::PendingScriptReader;
                iss.nodeId = bp.passes[i].id;
                iss.message = "RenderPipelineCompiler: pass '" + bp.passes[i].id + "' reads '" + read +
                              "', which waiting pass '" + source->second +
                              "' may write; it waits with that pass until the project's scripts are built.";
                bp.issues.push_back(std::move(iss));
                waiting.emplace_back(bp.passes[i].id, keptJson[i]);
                bp.passes.erase(bp.passes.begin() + static_cast<std::ptrdiff_t>(i));
                keptJson.erase(keptJson.begin() + static_cast<std::ptrdiff_t>(i));
                moved = true;
                break;
            }
        }
    }

    const auto writer = WaitingPassWrites(waiting, registry);
    const auto keptWrites = KeptWritesBefore(bp, keptJson, registry, bp.passes.size());
    std::string finalColor = Names::View::Resolve;
    for (const auto& output : bp.outputs)
        if (output.name == Names::Output::FinalColor)
            finalColor = output.resourceRef;
    if (const auto source = writer.find(finalColor); source != writer.end() && keptWrites.count(finalColor) == 0)
    {
        PipelineIssue iss{};
        iss.severity = PipelineIssueSeverity::Info;
        iss.code = PipelineIssueCode::PendingScriptOutput;
        iss.nodeId = source->second;
        iss.message = "RenderPipelineCompiler: the pipeline's output '" + finalColor + "' comes from waiting pass '" +
                      source->second + "', so nothing of it draws until the project's scripts are built.";
        bp.issues.push_back(std::move(iss));
    }
}

// One "inputGates" entry: the key must name one of the pass's inputs and the
// value must be a PostProcessSettings gate name (a gate reads 1.0 or 0.0; a value
// field would drop the input whenever the value is near zero). Empty when valid.
std::string InputGateIssue(const nlohmann::json& pass, const std::string& input, const nlohmann::json& gate,
                           const std::vector<std::string>& gateNames)
{
    std::vector<std::string> inputKeys;
    if (auto it = pass.find("inputs"); it != pass.end() && it->is_object())
        for (auto e = it->begin(); e != it->end(); ++e)
            inputKeys.push_back(e.key());
    if (std::find(inputKeys.begin(), inputKeys.end(), input) == inputKeys.end())
        return "inputGates input '" + input + "' is not one of this pass's inputs" +
               NearestNameSuffix(input, inputKeys);
    if (!gate.is_string())
        return "inputGates gate for input '" + input + "' must be a gate name string, not " + gate.dump();
    const std::string name = gate.get<std::string>();
    if (!::GameEngine::Engine::Renderer::PostProcessSettings::IsGateName(name))
        return "inputGates gate '" + name + "' for input '" + input + "' is not a PostProcessSettings gate" +
               NearestNameSuffix(name, gateNames);
    return {};
}

// The reflection of package `pkg` from `metaByPackage`, else from `preloadedMeta`, else from the disk,
// kept in `metaByPackage`. Null with `error` set when the disk read fails. The pointer is valid until
// the next insert into `metaByPackage`.
const ShaderMeta* FindShaderMeta(const std::string& pkg, const ShaderMetaLookup& preloadedMeta,
                                 FlatMap<std::string, ShaderMeta>& metaByPackage, std::string& error)
{
    if (const ShaderMeta* known = metaByPackage.Find(pkg))
        return known;
    ShaderMeta meta;
    if (!preloadedMeta || !preloadedMeta(pkg, meta))
    {
        // The reflection is the same in every source kind, so the stage bytes' form does not matter.
        ShaderPackage package;
        if (!LoadShaderPkg(pkg, ShaderSourceKind::SpirV, package, &error))
            return nullptr;
        meta = std::move(package.meta);
    }
    return &metaByPackage.InsertOrAssign(pkg, std::move(meta));
}

enum class ShaderSourceOverride : uint8_t
{
    Kept,
    Omitted,
    Malformed,
};

// Merges the pass's shaderSourceOverrides entry for `sourceKey` ("spirv" or "wgsl") into the pass and
// removes the overrides. Omitted: the entry is null, so the pass is left out for this source kind.
ShaderSourceOverride ResolveShaderSourceOverride(nlohmann::json& pass, const char* sourceKey)
{
    const auto variants = pass.find("shaderSourceOverrides");
    if (variants == pass.end())
        return ShaderSourceOverride::Kept;
    bool valid = variants->is_object();
    if (valid)
        for (auto it = variants->begin(); it != variants->end(); ++it)
            valid = valid && (it.key() == "spirv" || it.key() == "wgsl") && (it->is_object() || it->is_null());
    if (!valid)
        return ShaderSourceOverride::Malformed;
    const auto selected = variants->find(sourceKey);
    if (selected != variants->end())
    {
        if (selected->is_null())
            return ShaderSourceOverride::Omitted;
        const auto patch = *selected;
        pass.erase("shaderSourceOverrides");
        pass.merge_patch(patch);
    }
    pass.erase("shaderSourceOverrides");
    return ShaderSourceOverride::Kept;
}

const char* ShaderSourceKey(ShaderSourceKind sourceKind)
{
    return sourceKind == ShaderSourceKind::Wgsl ? "wgsl" : "spirv";
}

void ValidateBlueprint(RenderPipelineBlueprint& bp, const RenderPipelineNodeRegistry& registry,
                       const ShaderMetaLookup& preloadedMeta)
{
    auto addIssue = [&](PipelineIssueSeverity sev, std::string msg, std::string nodeId = std::string())
    {
        PipelineIssue iss{};
        iss.severity = sev;
        iss.message = std::move(msg);
        iss.nodeId = std::move(nodeId);
        bp.issues.push_back(std::move(iss));
    };

    // Parse each accepted pass's JSON once; reused by every sub-check.
    std::vector<nlohmann::json> passJson(bp.passes.size());
    for (size_t i = 0; i < bp.passes.size(); ++i)
    {
        try
        {
            passJson[i] = nlohmann::json::parse(bp.passes[i].passJson);
        }
        catch (...)
        {
            passJson[i] = nlohmann::json::object();
        }
    }

    // ── Valid-target set: builtins ∪ declared resources ∪ scan-derived publishes ──
    // NormalRoughness is engine-published (ReflectionsProvider creates it when a
    // blueprint predates the SSR targets), so it is valid without a declaration.
    std::unordered_set<std::string> validSet = {Names::View::Color, Names::View::Depth,
                                                Names::View::Resolve, Names::View::DepthResolved,
                                                Names::View::NormalRoughness, Names::View::BloomSampleDelta};
    for (const auto& r : bp.resources)
        validSet.insert(r.name);
    for (size_t i = 0; i < bp.passes.size(); ++i)
    {
        const RenderPipelineNodeResourceFields& fields = ResourceFieldsForType(registry, bp.passes[i].type);
        const auto& pj = passJson[i];
        for (const auto& [key, defaultName] : fields.PublishKeys)
        {
            std::string val;
            if (JsonTryGetString(pj, key.c_str(), val) && !val.empty())
                validSet.insert(val);
            else if (!defaultName.empty())
                validSet.insert(defaultName);
        }
        for (const auto& s : fields.StaticPublishNames)
            validSet.insert(s);
    }
    const std::vector<std::string> validList(validSet.begin(), validSet.end());

    // ── Declaration order (Error): a pass that builds its binding table when it is declared reads the
    // view parameters only if ViewParamsUpload was declared before it; declared after, it binds the
    // fallback block (an identity projection, no TAAU mip bias) without a word.
    {
        const auto upload = std::find_if(bp.passes.begin(), bp.passes.end(),
                                         [](const auto& p) { return p.type == "ViewParamsUpload"; });
        for (auto it = bp.passes.begin(); upload != bp.passes.end() && it != upload; ++it)
            if (it->type == "DepthPrepass" || it->type == "WorldRender")
                addIssue(PipelineIssueSeverity::Error,
                         "pass '" + it->id + "'" + (it->id == it->type ? std::string() : " (" + it->type + ")") +
                             " is declared before '" + upload->id + "', so it draws without the view's camera; move '" +
                             upload->id + "' above it",
                         it->id);
    }

    // ── Resource-ref resolution (Error) ──
    auto checkRef = [&](const std::string& value, const std::string& field, const std::string& nodeId)
    {
        if (value.empty() || validSet.count(value))
            return;
        addIssue(PipelineIssueSeverity::Error,
                 "unresolvable resource reference '" + value + "' in field '" + field + "'" +
                     NearestNameSuffix(value, validList),
                 nodeId);
    };

    for (size_t i = 0; i < bp.passes.size(); ++i)
    {
        const auto& pass = bp.passes[i];
        const auto& pj = passJson[i];
        {
            const RenderPipelineNodeResourceFields& rf = ResourceFieldsForType(registry, pass.type);
            for (const auto& key : rf.RefKeys)
            {
                std::string val;
                if (JsonTryGetString(pj, key.c_str(), val))
                    checkRef(val, key, pass.id);
            }
            for (const auto& key : rf.RefMapKeys)
            {
                auto it = pj.find(key);
                if (it == pj.end() || !it->is_object())
                    continue;
                for (auto e = it->begin(); e != it->end(); ++e)
                    if (e.value().is_string())
                        checkRef(e.value().get<std::string>(), key + "." + e.key(), pass.id);
            }
            if (!rf.InputKey.empty())
            {
                std::string val;
                if (JsonTryGetString(pj, rf.InputKey.c_str(), val) && !val.empty())
                {
                    std::vector<std::string> inputKeys;
                    if (auto it = pj.find("inputs"); it != pj.end() && it->is_object())
                        for (auto e = it->begin(); e != it->end(); ++e)
                            inputKeys.push_back(e.key());
                    if (std::find(inputKeys.begin(), inputKeys.end(), val) == inputKeys.end())
                        addIssue(PipelineIssueSeverity::Error,
                                 "'" + rf.InputKey + "' names '" + val +
                                     "' which is not one of this pass's inputs" +
                                     NearestNameSuffix(val, inputKeys),
                                 pass.id);
                }
            }
        }

        std::string crt;
        if (JsonTryGetString(pj, "colorResolveTarget", crt))
            checkRef(crt, "colorResolveTarget", pass.id);
    }

    // Explicit pipeline outputs (the FinalColor default lands later and is a builtin).
    for (const auto& o : bp.outputs)
        checkRef(o.resourceRef, "outputs." + o.name, std::string());

    // ── Resource blob schema (Error): kind/scope/format/usage/memoryUsage/size ──
    for (const auto& r : bp.resources)
    {
        nlohmann::json jr;
        try
        {
            jr = nlohmann::json::parse(r.resourceJson);
        }
        catch (...)
        {
            continue;
        }
        if (!jr.is_object())
            continue;

        // A resource-local size variable named like a constant shadows it (locals
        // seed after constants in the evaluator, so the local silently wins).
        if (!bp.constants.empty() && jr.contains("size") && jr["size"].is_object())
        {
            const auto& szv = jr["size"];
            if (szv.contains("variables") && szv["variables"].is_object())
                for (auto e = szv["variables"].begin(); e != szv["variables"].end(); ++e)
                    if (bp.constants.count(e.key()))
                        addIssue(PipelineIssueSeverity::Warning,
                                 "resource '" + r.name + "': size variable '" + e.key() +
                                     "' shadows pipeline constant '" + e.key() + "'");
        }

        std::string kind;
        (void)JsonTryGetString(jr, "kind", kind);
        const std::string kindLower = ToLowerAscii(kind);
        const bool isBuffer = kindLower == "buffer";
        const bool isTexture = kindLower.empty() || kindLower == "texture";
        if (!isBuffer && !isTexture)
            addIssue(PipelineIssueSeverity::Error,
                     "resource '" + r.name + "': unknown kind '" + kind +
                         "' (expected Texture or Buffer).");

        std::string scope;
        if (JsonTryGetString(jr, "scope", scope) && !scope.empty())
        {
            const std::string s = ToLowerAscii(scope);
            if (s != "frame" && s != "perview" && s != "view")
                addIssue(PipelineIssueSeverity::Error,
                         "resource '" + r.name + "': unknown scope '" + scope +
                             "' (expected Frame or PerView).");
        }

        if (isTexture)
        {
            std::string fmt;
            if (JsonTryGetString(jr, "format", fmt) && !TextureFormatTable().Lookup(fmt))
                addIssue(PipelineIssueSeverity::Error,
                         "resource '" + r.name + "': unknown texture format '" + fmt + "'" +
                             NearestNameSuffix(fmt, TextureFormatTable().ValidNames()));
        }

        if (jr.contains("usage") && jr["usage"].is_array())
        {
            for (const auto& u : jr["usage"])
            {
                if (!u.is_string())
                    continue;
                const std::string us = u.get<std::string>();
                const bool ok = isBuffer ? static_cast<bool>(BufferUsageTable().Lookup(us))
                                         : static_cast<bool>(TextureUsageTable().Lookup(us));
                if (!ok)
                    addIssue(PipelineIssueSeverity::Error,
                             "resource '" + r.name + "': unknown " +
                                 (isBuffer ? "buffer" : "texture") + " usage '" + us + "'");
            }
        }

        if (isBuffer)
        {
            std::string mem;
            if (JsonTryGetString(jr, "memoryUsage", mem) && !BufferMemoryUsageTable().Lookup(mem))
                addIssue(PipelineIssueSeverity::Error,
                         "resource '" + r.name + "': unknown memoryUsage '" + mem + "'" +
                             NearestNameSuffix(mem, BufferMemoryUsageTable().ValidNames()));

            if (jr.contains("size") && jr["size"].is_object())
            {
                const auto& sz = jr["size"];
                std::string expr;
                if (JsonTryGetString(sz, "expression", expr) && !expr.empty())
                {
                    // Constants + reserved extent identifiers are in scope at
                    // materialize time; seed constants first, then dummy positive
                    // extents, so the expression is checked for parse/identifier
                    // validity (not for a specific result). Mirrors MaterializeBuffer.
                    std::unordered_map<std::string, double> vars;
                    for (const auto& kv : bp.constants)
                        vars[kv.first] = kv.second;
                    vars["renderWidth"] = 1920.0;
                    vars["renderHeight"] = 1080.0;
                    std::string err;
                    if (sz.contains("variables"))
                        (void)Expr::BuildVariables(sz["variables"], vars, &err);
                    double v = 0.0;
                    if (!Expr::EvalExpression(expr, vars, v, &err))
                        addIssue(PipelineIssueSeverity::Error,
                                 "resource '" + r.name + "': size expression '" + expr +
                                     "' failed to evaluate: " + err);
                }
            }
        }
    }

    // ── shaderPkg existence + reflection ──
    // Empty-but-required is a structural Error. Absolute package paths need no
    // resolver and are always validated. Skip relative paths only when no
    // resolver is installed (in-code/test compile); a configured resolver that
    // cannot load a required package is an Error. Loaded metadata gates binding
    // and push-constant name checks (Warning).
    const bool resolverUnconfigured = !Utils::HasShaderPathResolver();
    if (resolverUnconfigured)
        Logger::Log::Info("[RenderPipeline] relative shaderpkg checks skipped: no shader path "
                          "resolver configured (in-code/test compile).");
    FlatMap<std::string, ShaderMeta> metaByPackage;
    for (size_t i = 0; i < bp.passes.size(); ++i)
    {
        const auto& pass = bp.passes[i];
        const auto& pj = passJson[i];

        std::string pkg;
        const bool hasPkg = JsonTryGetString(pj, "shaderPkg", pkg) && !pkg.empty();
        if (RequiresShaderPkg(pass.type) && !hasPkg)
        {
            addIssue(PipelineIssueSeverity::Error,
                     "pass type '" + pass.type + "' requires a non-empty 'shaderPkg'.", pass.id);
            continue;
        }
        if (!hasPkg || (resolverUnconfigured && !std::filesystem::path(pkg).is_absolute()))
            continue;

        std::string err;
        const ShaderMeta* meta = FindShaderMeta(pkg, preloadedMeta, metaByPackage, err);
        if (!meta)
        {
            addIssue(PipelineIssueSeverity::Error,
                     "shaderPkg '" + pkg + "' could not be loaded: " +
                         (err.empty() ? std::string("not found") : err) +
                         ". Point the pass's \"shaderPkg\" at a package that exists, or delete the pass and "
                         "point the passes that read its output at the input it read.",
                     pass.id);
            continue;
        }

        std::vector<std::string> pcMembers;
        for (const auto& pc : meta->PushConstants)
            for (const auto& mem : pc.Block.Members)
                pcMembers.push_back(mem.Name);
        std::vector<std::string> set0;
        for (const auto& setMeta : meta->Sets)
            if (setMeta.Set == 0)
                for (const auto& b : setMeta.Bindings)
                    set0.push_back(b.Name);
        const std::unordered_set<std::string> pcSet(pcMembers.begin(), pcMembers.end());
        const std::unordered_set<std::string> bindSet(set0.begin(), set0.end());

        if (auto it = pj.find("pushConstants"); it != pj.end() && it->is_object())
            for (auto e = it->begin(); e != it->end(); ++e)
                if (!pcSet.count(e.key()))
                    addIssue(PipelineIssueSeverity::Warning,
                             "pushConstant '" + e.key() + "' is not a push-constant member of "
                             "shaderpkg '" + pkg + "'" + NearestNameSuffix(e.key(), pcMembers),
                             pass.id);

        for (const char* refKey : {"inputs", "buffers"})
            if (auto it = pj.find(refKey); it != pj.end() && it->is_object())
                for (auto e = it->begin(); e != it->end(); ++e)
                    if (!bindSet.count(e.key()))
                        addIssue(PipelineIssueSeverity::Warning,
                                 std::string(refKey) + " binding '" + e.key() +
                                     "' has no set-0 binding in shaderpkg '" + pkg + "'" +
                                     NearestNameSuffix(e.key(), set0),
                                 pass.id);
    }

    // ── skipWhen keys vs the PostProcessSettings read registry, inputGates vs the
    // pass's inputs and the settings gates (Warning) ──
    using ::GameEngine::Engine::Renderer::PostProcessSettings;
    const std::vector<std::string>& readable = PostProcessSettings::ReadableFieldNames();
    const std::unordered_set<std::string> readableSet(readable.begin(), readable.end());
    std::vector<std::string> gateNames;
    for (const std::string& name : readable)
    {
        if (PostProcessSettings::IsGateName(name))
            gateNames.push_back(name);
    }
    for (size_t i = 0; i < bp.passes.size(); ++i)
    {
        const auto& pj = passJson[i];
        if (auto it = pj.find("skipWhen"); it != pj.end() && it->is_object())
            for (auto e = it->begin(); e != it->end(); ++e)
                if (!readableSet.count(e.key()))
                    addIssue(PipelineIssueSeverity::Warning,
                             "skipWhen field '" + e.key() +
                                 "' is not a readable PostProcessSettings field" +
                                 NearestNameSuffix(e.key(), readable),
                             bp.passes[i].id);
        if (auto it = pj.find("inputGates"); it != pj.end() && it->is_object())
            for (auto e = it->begin(); e != it->end(); ++e)
                if (std::string issue = InputGateIssue(pj, e.key(), *e, gateNames); !issue.empty())
                    addIssue(PipelineIssueSeverity::Warning, std::move(issue), bp.passes[i].id);
    }

    // ── Deprecated node type keys (Warning) ──
    // The Q4 rename made "ShadowMap" the canonical key for the shadow node; the
    // old "CascadedShadowMap" key still parses (registered as an alias) but nudges
    // authors to migrate so the legacy string can eventually be retired.
    for (const auto& pass : bp.passes)
        if (pass.type == "CascadedShadowMap")
            addIssue(PipelineIssueSeverity::Warning,
                     "legacy node type key 'CascadedShadowMap' — rename to 'ShadowMap'", pass.id);

    // ── Duplicate published output names (Warning) ──
    // Re-publishing a blackboard name is a first-class pattern here (the frame
    // resources are last-writer-wins), so this catches only the genuine mistake:
    // two passes inventing the SAME name that has no backing target, silently
    // clobbering each other. Two narrowings keep it from flagging by-design work:
    //   - Only ENABLED passes publish at runtime; a disabled pass (e.g. an
    //     off-by-default debug overlay) declares nothing and cannot collide.
    //   - A per-view builtin or a name the blueprint DECLARES in "resources" is a
    //     sanctioned shared target that post-FX chains write from several passes
    //     on purpose: bloom-pyramid ping-pong, in-place fog/flare composites, the
    //     final-resolve chain. The declaration is the author's intent — not a bug.
    const std::unordered_set<std::string> sanctionedTargets = [&]
    {
        std::unordered_set<std::string> s = {
            Names::View::Color,          Names::View::Depth, Names::View::Resolve,
            Names::View::DepthResolved,  Names::View::GTAO,  Names::View::HZB,
            Names::View::EffectiveColor, Names::View::DepthResolvedPostOcean,
            Names::View::NormalRoughness};
        for (const auto& r : bp.resources)
            s.insert(r.name);
        return s;
    }();

    std::unordered_map<std::string, int> pubCounts;
    for (size_t i = 0; i < bp.passes.size(); ++i)
    {
        if (!bp.passes[i].enabled)
            continue;
        const RenderPipelineNodeResourceFields& fields = ResourceFieldsForType(registry, bp.passes[i].type);
        const auto& pj = passJson[i];
        for (const auto& [key, defaultName] : fields.PublishKeys)
        {
            std::string val;
            if (JsonTryGetString(pj, key.c_str(), val) && !val.empty())
                pubCounts[val]++;
        }
        for (const auto& s : fields.StaticPublishNames)
            pubCounts[s]++;
    }
    for (const auto& kv : pubCounts)
        if (kv.second > 1 && !sanctionedTargets.count(kv.first))
            addIssue(PipelineIssueSeverity::Warning,
                     "output name '" + kv.first + "' is published by " +
                         std::to_string(kv.second) + " enabled passes with no declared target.");
}

// One raster target of the internal-resolution split: a caller target's format at the internal extent
// and the view's sample count. Pool-persistent, so the HZB, AO and fog consumers bind a physical at
// declaration.
Rendering::RenderGraph::RGTexture ImportRenderScaleSplitTarget(Rendering::RenderGraph::RGFrame& frame,
                                                               Rendering::ViewId viewId, const char* suffix,
                                                               const Rendering::RenderGraph::RGResourceDesc& caller,
                                                               uint32_t usage, uint32_t samples, uint32_t width,
                                                               uint32_t height)
{
    Rendering::TextureDesc textureDesc{};
    textureDesc.width = width;
    textureDesc.height = height;
    textureDesc.depth = 1;
    textureDesc.mipLevels = 1;
    textureDesc.arrayLayers = 1;
    textureDesc.sampleCount = samples;
    textureDesc.format = caller.Format;
    textureDesc.usage = usage;
    const std::string name =
        "Pipeline.RenderScaleSplit.View" + std::to_string(static_cast<uint32_t>(viewId)) + "." + suffix;
    textureDesc.debugName = name.c_str();
    return frame.ImportPersistentTexture(name.c_str(), textureDesc);
}

// Why a multisampled view cannot split, or null when it can. The crossing writes the caller's
// single-sample resolve at the display extent and reads the single-sample View.DepthResolved.
const char* MultisampledSplitBlocker(const Rendering::RenderGraph::RGFrame& frame,
                                     Rendering::RenderGraph::RGTexture callerResolve, uint32_t width,
                                     uint32_t height, bool hasDepthResolve)
{
    if (!callerResolve.IsValid())
        return "the view has no single-sample resolve target";
    const auto& resolveDesc = frame.Graph().ResourceDesc(callerResolve.Id);
    if (resolveDesc.SampleCount > 1 || resolveDesc.Width != width || resolveDesc.Height != height)
        return "the view's resolve target is not a single-sample target at the view's extent";
    if (!hasDepthResolve)
        return "the pipeline has no enabled DepthResolve node to resolve the multisampled depth";
    return nullptr;
}

} // namespace

RenderPipelineBlueprint RenderPipelineCompiler::Compile(const GameEngine::RenderPipelineAsset& asset,
                                                        const RenderPipelineNodeRegistry& registry,
                                                        Rendering::ShaderSourceKind sourceKind,
                                                        UnknownPassTypes unknownPassTypes,
                                                        const ShaderMetaLookup& preloadedMeta) const
{
    RenderPipelineBlueprint bp{};
    bp.schemaVersion = asset.GetDocument().schemaVersion;
    bp.pipelineName = asset.GetDocument().pipelineName;
    bp.sourcePath = asset.GetPath().string();
    const char* sourceKey = ShaderSourceKey(sourceKind);
    bp.contentHash = HashString(asset.GetDocument().jsonText + sourceKey);

    // Carry forward any asset-loader errors as pipeline issues.
    for (const auto& e : asset.GetErrors())
    {
        PipelineIssue iss{};
        iss.severity = PipelineIssueSeverity::Error;
        iss.message = e;
        bp.issues.push_back(std::move(iss));
    }

    if (asset.GetDocument().jsonText.empty())
    {
        PipelineIssue iss{};
        iss.severity = PipelineIssueSeverity::Error;
        iss.message = "RenderPipelineCompiler: asset contains no JSON text.";
        bp.issues.push_back(std::move(iss));
        return bp;
    }

    nlohmann::json root;
    try
    {
        root = nlohmann::json::parse(asset.GetDocument().jsonText);
    }
    catch (const std::exception& e)
    {
        PipelineIssue iss{};
        iss.severity = PipelineIssueSeverity::Error;
        iss.message = std::string("RenderPipelineCompiler: JSON parse failed: ") + e.what();
        bp.issues.push_back(std::move(iss));
        return bp;
    }

    if (!root.is_object())
    {
        PipelineIssue iss{};
        iss.severity = PipelineIssueSeverity::Error;
        iss.message = "RenderPipelineCompiler: root must be a JSON object.";
        bp.issues.push_back(std::move(iss));
        return bp;
    }

    // Read canonical schemaVersion/pipelineName from compiler view (authoritative for runtime)
    {
        int v = 0;
        if (root.contains("schemaVersion") && root["schemaVersion"].is_number_integer())
            v = root["schemaVersion"].get<int>();
        bp.schemaVersion = v > 0 ? (uint32_t)v : 0u;
        if (bp.schemaVersion != 2u)
        {
            PipelineIssue iss{};
            iss.severity = PipelineIssueSeverity::Error;
            iss.message = "RenderPipelineCompiler: unsupported schemaVersion (expected 2).";
            bp.issues.push_back(std::move(iss));
        }

        std::string n;
        if (JsonTryGetString(root, "pipelineName", n) || JsonTryGetString(root, "name", n))
            bp.pipelineName = n;
        if (bp.pipelineName.empty())
            bp.pipelineName = asset.GetPath().stem().string();
    }

    // Outputs (optional)
    if (root.contains("outputs") && root["outputs"].is_object())
    {
        for (auto it = root["outputs"].begin(); it != root["outputs"].end(); ++it)
        {
            if (!it.value().is_string())
                continue;
            RenderPipelineBlueprint::Output o{};
            o.name = it.key();
            o.resourceRef = it.value().get<std::string>();
            bp.outputs.push_back(std::move(o));
        }
    }

    // Resources (optional) – stored as JSON blobs for the runtime instance
    if (root.contains("resources"))
    {
        const auto& r = root["resources"];
        if (r.is_object())
        {
            for (auto it = r.begin(); it != r.end(); ++it)
            {
                RenderPipelineBlueprint::Resource res{};
                res.name = it.key();
                res.resourceJson = it.value().dump();
                bp.resources.push_back(std::move(res));
            }
        }
        else if (r.is_array())
        {
            for (size_t i = 0; i < r.size(); ++i)
            {
                const auto& e = r[i];
                if (!e.is_object())
                    continue;
                std::string name;
                if (!JsonTryGetString(e, "name", name) || name.empty())
                    continue;
                RenderPipelineBlueprint::Resource res{};
                res.name = name;
                res.resourceJson = e.dump();
                bp.resources.push_back(std::move(res));
            }
        }
        else
        {
            PipelineIssue iss{};
            iss.severity = PipelineIssueSeverity::Error;
            iss.message = "RenderPipelineCompiler: 'resources' must be an object or array.";
            bp.issues.push_back(std::move(iss));
        }
    }

    // Constants (optional): top-level name->number, seeded into the buffer-size
    // expression evaluator before per-resource variables (S2.2). Reserved extent
    // identifiers and non-number values are rejected here; a resource-local
    // variable shadowing a constant is a Warning raised in ValidateBlueprint.
    if (root.contains("constants"))
    {
        const auto& c = root["constants"];
        if (!c.is_object())
        {
            PipelineIssue iss{};
            iss.severity = PipelineIssueSeverity::Error;
            iss.message = "RenderPipelineCompiler: 'constants' must be a JSON object.";
            bp.issues.push_back(std::move(iss));
        }
        else
        {
            for (auto it = c.begin(); it != c.end(); ++it)
            {
                const std::string& name = it.key();
                if (name == "renderWidth" || name == "renderHeight")
                {
                    PipelineIssue iss{};
                    iss.severity = PipelineIssueSeverity::Error;
                    iss.message = "RenderPipelineCompiler: constant '" + name +
                                  "' shadows a reserved extent identifier.";
                    bp.issues.push_back(std::move(iss));
                    continue;
                }
                if (!it.value().is_number())
                {
                    PipelineIssue iss{};
                    iss.severity = PipelineIssueSeverity::Error;
                    iss.message =
                        "RenderPipelineCompiler: constant '" + name + "' must be a number.";
                    bp.issues.push_back(std::move(iss));
                    continue;
                }
                bp.constants[name] = it.value().get<double>();
            }
        }
    }

    // Passes (required, schemaVersion 2)
    if (!root.contains("passes") || !root["passes"].is_array())
    {
        PipelineIssue iss{};
        iss.severity = PipelineIssueSeverity::Error;
        iss.message = "RenderPipelineCompiler: missing required array field 'passes'.";
        bp.issues.push_back(std::move(iss));
        return bp;
    }

    std::unordered_set<std::string> ids;
    const auto& passes = root["passes"];
    // Passes left out for the project's scripts, with their JSON, for the readers that wait on them.
    std::vector<std::pair<std::string, nlohmann::json>> waitingPasses;
    for (size_t i = 0; i < passes.size(); ++i)
    {
        auto p = passes[i];
        if (!p.is_object())
        {
            PipelineIssue iss{};
            iss.severity = PipelineIssueSeverity::Error;
            iss.message = "RenderPipelineCompiler: pass entry must be an object (passes[" + std::to_string(i) + "]).";
            bp.issues.push_back(std::move(iss));
            continue;
        }

        const ShaderSourceOverride sourceOverride = ResolveShaderSourceOverride(p, sourceKey);
        if (sourceOverride == ShaderSourceOverride::Malformed)
        {
            PipelineIssue issue{};
            issue.severity = PipelineIssueSeverity::Error;
            issue.message = "RenderPipelineCompiler: shaderSourceOverrides requires spirv/wgsl objects or null (passes[" + std::to_string(i) + "]).";
            bp.issues.push_back(std::move(issue));
            continue;
        }
        if (sourceOverride == ShaderSourceOverride::Omitted)
            continue;

        std::string id;
        std::string type;
        if (!JsonTryGetString(p, "id", id) || id.empty())
        {
            PipelineIssue iss{};
            iss.severity = PipelineIssueSeverity::Error;
            iss.message = "RenderPipelineCompiler: pass is missing string field 'id' (passes[" + std::to_string(i) + "]).";
            bp.issues.push_back(std::move(iss));
            continue;
        }
        if (!JsonTryGetString(p, "type", type) || type.empty())
        {
            PipelineIssue iss{};
            iss.severity = PipelineIssueSeverity::Error;
            iss.nodeId = id;
            iss.message = "RenderPipelineCompiler: pass '" + id + "' is missing string field 'type'.";
            bp.issues.push_back(std::move(iss));
            continue;
        }

        if (!ids.insert(id).second)
        {
            PipelineIssue iss{};
            iss.severity = PipelineIssueSeverity::Error;
            iss.nodeId = id;
            iss.message = "RenderPipelineCompiler: duplicate pass id '" + id + "'.";
            bp.issues.push_back(std::move(iss));
            continue;
        }

        const auto* typeInfo = registry.Find(type);
        if (!typeInfo)
        {
            PipelineIssue iss{};
            iss.severity = PipelineIssueSeverity::Error;
            iss.nodeId = id;
            if (unknownPassTypes == UnknownPassTypes::PendingScripts)
            {
                iss.severity = PipelineIssueSeverity::Info;
                iss.code = PipelineIssueCode::PendingScriptPass;
                iss.message = "RenderPipelineCompiler: pass '" + id + "' (type '" + type +
                              "') waits for the project's scripts to register its type; it joins once they are "
                              "built.";
                waitingPasses.emplace_back(id, p);
            }
            else
            {
                iss.code = PipelineIssueCode::UnknownPassType;
                iss.message = "RenderPipelineCompiler: unknown pass type '" + type + "' (node '" + id +
                              "'): no loaded scripts register it; build the scripts or install the package that "
                              "provides it, or remove the node.";
            }
            bp.issues.push_back(std::move(iss));
            continue;
        }

        RenderPipelineBlueprint::Pass nb{};
        nb.id = id;
        nb.type = type;
        nb.perView = typeInfo->perView;
        nb.enabled = true;
        (void)JsonTryGetBool(p, "enabled", nb.enabled);
        nb.passJson = p.dump();

        // The pass that resolves the scene color into a sampled target (e.g. the
        // World pass -> "SceneColor") defines where the FX chain reads from. Record
        // it so MSAA-off can render the scene straight into that target.
        if (std::string crt; JsonTryGetString(p, "colorResolveTarget", crt) && !crt.empty())
            bp.worldColorResolveTargetRef = crt;

        std::string phaseStr;
        if (JsonTryGetString(p, "phase", phaseStr))
            nb.phase = ParsePassPhase(phaseStr);

        if (p.contains("tags") && p["tags"].is_array())
        {
            for (const auto& t : p["tags"])
            {
                if (t.is_string())
                    nb.tags.push_back(GameEngine::HashStringId(t.get<std::string>()));
            }
        }
        if (p.contains("after") && p["after"].is_array())
        {
            for (const auto& t : p["after"])
            {
                if (t.is_string())
                    nb.afterTags.push_back(GameEngine::HashStringId(t.get<std::string>()));
            }
        }

        bp.passes.push_back(std::move(nb));
    }

    DeclareDepthPrepassFeedersFirst(bp, registry);

    // Semantic validation: resource refs, resource-blob enums, shaderpkg
    // reflection, skipWhen fields. Errors reject the blueprint wholesale at the
    // FrameOrchestrator enforcement point (last-good pipeline kept).
    if (!waitingPasses.empty())
        LeaveOutPassesThatReadWaitingOnes(bp, registry, std::move(waitingPasses));

    ValidateBlueprint(bp, registry, preloadedMeta);

    // Default outputs if not specified
    if (bp.outputs.empty())
    {
        RenderPipelineBlueprint::Output o{};
        o.name = Names::Output::FinalColor;
        o.resourceRef = Names::View::Resolve;
        bp.outputs.push_back(std::move(o));
    }

    // The same source without its waiting passes is a different blueprint from the full graph.
    std::string pendingIds;
    for (const auto& issue : bp.issues)
    {
        if (issue.code == PipelineIssueCode::PendingScriptPass || issue.code == PipelineIssueCode::PendingScriptReader)
            pendingIds += issue.nodeId + '\n';
    }
    if (!pendingIds.empty())
        bp.contentHash ^= HashString("pending script passes\n" + pendingIds);

    return bp;
}

std::vector<std::string> RenderPipelineCompiler::ShaderPackagesOf(const GameEngine::RenderPipelineAsset& asset,
                                                                  Rendering::ShaderSourceKind sourceKind)
{
    std::vector<std::string> packages;
    const nlohmann::json root = nlohmann::json::parse(asset.GetDocument().jsonText, nullptr, false);
    if (!root.is_object())
        return packages;
    const auto passes = root.find("passes");
    if (passes == root.end() || !passes->is_array())
        return packages;
    const char* sourceKey = ShaderSourceKey(sourceKind);
    for (nlohmann::json pass : *passes)
    {
        if (!pass.is_object() || ResolveShaderSourceOverride(pass, sourceKey) != ShaderSourceOverride::Kept)
            continue;
        std::string package;
        if (JsonTryGetString(pass, "shaderPkg", package) && !package.empty() &&
            std::find(packages.begin(), packages.end(), package) == packages.end())
            packages.push_back(std::move(package));
    }
    return packages;
}

struct RenderPipelineInstance::ResourceDescription
{
    enum class Kind
    {
        Invalid,
        Texture,
        Buffer
    };
    Kind Type = Kind::Invalid;
    bool PerView = false;
    // "preExposed": the texture holds scene colour stored at the frame's pre-exposure P
    // (pre_exposure_snapshot.comp); a reader of absolute values divides by P.
    bool PreExposed = false;
    bool AbsoluteExtent = false;
    bool OutputExtent = false;
    float ScaleX = 1.0f;
    float ScaleY = 1.0f;
    Rendering::TextureDesc Texture{};
    nlohmann::json Buffer;

    explicit ResourceDescription(const std::string& resourceJson)
    {
        nlohmann::json jr;
        try
        {
            GE_CPU_PROFILE_SCOPE("RenderPipeline.ParseResource");
            jr = nlohmann::json::parse(resourceJson);
        }
        catch (...)
        {
            return;
        }
        if (!jr.is_object())
            return;
        std::string kind;
        std::string scope;
        (void)JsonTryGetString(jr, "kind", kind);
        (void)JsonTryGetString(jr, "scope", scope);
        kind = ToLowerAscii(kind);
        scope = ToLowerAscii(scope);
        PerView = scope == "perview" || scope == "view";
        if (kind == "buffer")
        {
            Type = Kind::Buffer;
            Buffer = std::move(jr);
            return;
        }
        if (!kind.empty() && kind != "texture")
            return;
        Type = Kind::Texture;
        if (jr.contains("preExposed") && jr["preExposed"].is_boolean())
            PreExposed = jr["preExposed"].get<bool>();

        Rendering::TextureFormat fmt = Rendering::TextureFormat::RGBA8_UNORM;
        std::string fmtStr;
        if (JsonTryGetString(jr, "format", fmtStr))
        {
            if (auto f = TextureFormatTable().Lookup(fmtStr))
                fmt = *f;
            else
                WarnUnknownEnumOnce("TextureFormat", fmtStr, TextureFormatTable().ValidNames());
        }

        uint32_t sampleCount = 1u;
        if (jr.contains("sampleCount") && jr["sampleCount"].is_number_integer())
        {
            const int sc = jr["sampleCount"].get<int>();
            if (sc > 0)
                sampleCount = (uint32_t)sc;
        }

        Rendering::TextureUsage usage =
            Rendering::TextureUsage::RenderTarget | Rendering::TextureUsage::ShaderResource;
        if (jr.contains("usage") && jr["usage"].is_array())
        {
            usage = Rendering::TextureUsage::None;
            for (const auto& u : jr["usage"])
            {
                if (!u.is_string())
                    continue;
                const std::string us = u.get<std::string>();
                if (auto f = TextureUsageTable().Lookup(us))
                    usage = usage | *f;
                else
                    WarnUnknownEnumOnce("TextureUsage", us, TextureUsageTable().ValidNames());
            }
        }
        // Color targets are CopyTexture sources for editor snapshots (bookmark
        // preview, last-presented). WebGPU requires COPY_SRC at creation.
        if ((usage & Rendering::TextureUsage::RenderTarget) != Rendering::TextureUsage::None)
            usage = usage | Rendering::TextureUsage::TransferSrc;

        float sx = 1.0f, sy = 1.0f;
        uint32_t absW = 0, absH = 0, absD = 1;
        uint32_t mipLevels = 1, arrayLayers = 1;
        bool useAbs = false;
        auto readPositiveU32 = [](const nlohmann::json& o, const char* key, uint32_t& out) -> bool
        {
            if (!o.contains(key) || !o[key].is_number_integer())
                return false;
            out = (uint32_t)std::max<int>(1, o[key].get<int>());
            return true;
        };
        readPositiveU32(jr, "mipLevels", mipLevels);
        readPositiveU32(jr, "mips", mipLevels);
        readPositiveU32(jr, "arrayLayers", arrayLayers);
        readPositiveU32(jr, "layers", arrayLayers);
        if (jr.contains("extent") && jr["extent"].is_object())
        {
            const auto& ex = jr["extent"];
            if (ex.contains("width") && ex.contains("height") && ex["width"].is_number_integer() &&
                ex["height"].is_number_integer())
            {
                absW = (uint32_t)std::max<int>(1, ex["width"].get<int>());
                absH = (uint32_t)std::max<int>(1, ex["height"].get<int>());
                useAbs = true;
            }
            readPositiveU32(ex, "depth", absD);
            readPositiveU32(ex, "mipLevels", mipLevels);
            readPositiveU32(ex, "mips", mipLevels);
            readPositiveU32(ex, "arrayLayers", arrayLayers);
            readPositiveU32(ex, "layers", arrayLayers);
            if (!useAbs && ex.contains("scale") && ex["scale"].is_array() && ex["scale"].size() >= 2)
            {
                if (ex["scale"][0].is_number())
                    sx = ex["scale"][0].get<float>();
                if (ex["scale"][1].is_number())
                    sy = ex["scale"][1].get<float>();
            }
            // Relative-extent basis: "render" (default — the internal raster
            // extent) or "output" (the display extent). Resources after the
            // crossing point declare "output" so a reduced internal resolution
            // never re-softens the upscaled image. Where that boundary sits IS the
            // set of resources declaring each basis: the crossing node belongs
            // between the last "render" and the first "output" resource.
            // Identical extents when the split is inactive — inert at scale 1.0.
            std::string basis;
            if (JsonTryGetString(ex, "basis", basis) && ToLowerAscii(basis) == "output")
            {
                OutputExtent = true;
            }
        }
        AbsoluteExtent = useAbs;
        ScaleX = sx;
        ScaleY = sy;
        Texture.width = absW;
        Texture.height = absH;
        Texture.depth = absD;
        Texture.mipLevels = mipLevels;
        Texture.arrayLayers = absD > 1 ? 1 : arrayLayers;
        Texture.format = static_cast<uint32_t>(fmt);
        Texture.usage = static_cast<uint32_t>(usage);
        Texture.sampleCount = absD > 1 ? 1u : sampleCount;
    }
};

RenderPipelineInstance::RenderPipelineInstance(Engine::Renderer::RenderServices& rs,
                                               const RenderPipelineNodeRegistry& registry)
    : m_Rs(rs), m_Registry(registry)
{
}

RenderPipelineInstance::~RenderPipelineInstance() = default;

void RenderPipelineInstance::SetBlueprint(RenderPipelineBlueprint blueprint)
{
    m_Blueprint = std::move(blueprint);
    m_BlueprintDirty = true;
    m_ResourceDescriptions.clear();
    m_ResourceDescriptions.reserve(m_Blueprint.resources.size());
    for (const auto& resource : m_Blueprint.resources)
    {
        // Resource names identify descriptions only within this blueprint.
        // Preserve first-entry lookup behavior for a manually supplied duplicate.
        if (!m_ResourceDescriptions.contains(resource.name))
            m_ResourceDescriptions.emplace(resource.name,
                std::make_unique<ResourceDescription>(resource.resourceJson));
    }
}

// ── RenderGraph declaration path ────────────────────────────────────────────────────

void RenderPipelineInstance::EnsureNodeInstances()
{
    // The Declare-path arm of the m_BlueprintDirty rebuild. Mirrors the
    // EnsureBuilt branch minus everything old-graph-coupled: no Disable(rg)
    // (this instance owns no old-graph passes), no resource-cache clears
    // (the blackboard is rebuilt every frame anyway).
    m_Outputs.clear();
    for (const auto& o : m_Blueprint.outputs)
        m_Outputs[o.name] = o.resourceRef;

    m_Nodes.clear();
    for (const auto& nb : m_Blueprint.passes)
    {
        const auto* typeInfo = m_Registry.Find(nb.type);
        if (!typeInfo || !typeInfo->factory)
        {
            Logger::Log::Error(
                "RenderPipeline: unknown pass type '{}' for node '{}' (pass will be skipped)",
                nb.type, nb.id);
            continue;
        }
        auto node = typeInfo->factory();
        if (!node)
            continue;
        std::string err;
        if (!node->Initialize(nb.id, nb.passJson, &err))
        {
            Logger::Log::Error("RenderPipeline: node '{}' (type '{}') failed to initialize: {}",
                               nb.id, nb.type, err);
            continue;
        }
        m_Nodes.emplace(nb.id, std::move(node));
    }
    for (const auto& nb : m_Blueprint.passes)
    {
        if (!nb.enabled)
            continue;
        if (m_Nodes.find(nb.id) == m_Nodes.end())
        {
            Logger::Log::Warning(
                "RenderPipeline: enabled pass '{}' (type '{}') has no node instance; it will "
                "not schedule any work",
                nb.id, nb.type);
        }
    }

    m_WorldRenderPassKeywords.reset();
    bool heightFogParamsWritten = false;
    bool depthPrepassSeen = false;
    for (const auto& nb : m_Blueprint.passes)
    {
        if (!nb.enabled || m_Nodes.find(nb.id) == m_Nodes.end())
            continue;
        if (nb.type == "WorldRender")
        {
            m_WorldRenderPassKeywords = Nodes::ParseWorldPassKeywords(nb.passJson);
            m_HeightFogParamsBeforeWorldRender = heightFogParamsWritten;
            m_DepthPrepassBeforeWorldRender = depthPrepassSeen;
            break;
        }
        depthPrepassSeen = depthPrepassSeen || nb.type == "DepthPrepass";
        ForEachWrite(ResourceFieldsForType(m_Registry, nb.type), nlohmann::json::parse(nb.passJson, nullptr, false),
                     [&heightFogParamsWritten](const std::string& name)
                     { heightFogParamsWritten = heightFogParamsWritten || name == Names::Res::HeightFogParams; });
    }

    m_PassNameCache.clear(); // names rebuilt lazily by DeclarePassName
    m_BlueprintDirty = false;
}

Rendering::RenderGraph::RGTexture RenderPipelineInstance::TableTexture(Rendering::ViewId viewId,
                                                               std::string_view name) const
{
    if (auto it = m_FrameResources.Textures.find({viewId, name});
        it != m_FrameResources.Textures.end())
        return it->second;
    if (viewId != 0)
    {
        if (auto it = m_FrameResources.Textures.find({0, name});
            it != m_FrameResources.Textures.end())
            return it->second;
    }
    return {};
}

Rendering::RenderGraph::RGBuffer RenderPipelineInstance::FindPreExposureSource(
    uint32_t textureResourceId) const
{
    const auto it = m_FrameResources.PreExposedTextures.find(textureResourceId);
    if (it == m_FrameResources.PreExposedTextures.end())
        return {};
    return TableBuffer(it->second, Names::Res::ExposureHistory).Graph;
}

PipelineBufferBindingRG RenderPipelineInstance::TableBuffer(Rendering::ViewId viewId,
                                                            std::string_view name) const
{
    if (auto it = m_FrameResources.Buffers.find({viewId, name});
        it != m_FrameResources.Buffers.end())
        return it->second;
    if (viewId != 0)
    {
        if (auto it = m_FrameResources.Buffers.find({0, name});
            it != m_FrameResources.Buffers.end())
            return it->second;
    }
    return {};
}

const std::string& RenderPipelineInstance::DeclarePassName(uint32_t nodeIdx,
                                                           Rendering::ViewId viewId,
                                                           const char* suffix)
{
    const uint64_t baseKey = MakePassNameKey(nodeIdx, static_cast<uint32_t>(viewId));
    auto it = m_PassNameCache.find(baseKey);
    if (it == m_PassNameCache.end())
    {
        it = m_PassNameCache
                 .emplace(baseKey, "Pipeline." + m_Blueprint.pipelineName + "." +
                                       m_Blueprint.passes[nodeIdx].id + ".View" +
                                       std::to_string(static_cast<uint32_t>(viewId)))
                 .first;
    }
    if (!suffix || !suffix[0])
        return it->second;
    return ResolveSuffixedPassName(nodeIdx, static_cast<uint32_t>(viewId), it->second, suffix);
}

void RenderPipelineInstance::TickDynamicResolution(Rendering::RenderGraph::RGFrame& frame,
                                                   float deltaTimeSeconds)
{
    if (m_Rs.GetDynamicResolutionMode() != DynamicResolutionMode::Dynamic)
        return;

    // DRS needs GPU-busy time, and the render graph's per-pass timestamps are
    // the only such measurement the engine has: the device's frameGpuPeriodMs
    // is a wall-clock PERIOD between frame ends, so it folds in vsync waits and
    // frame-limiter sleep and would have the controller chase idle time.
    // Dynamic mode therefore owns the profiler arm for as long as it is active.
    if (!frame.ProfilingEnabled())
    {
        frame.SetProfilingEnabled(true);
        if (!m_WarnedDrsArmedProfiler)
        {
            m_WarnedDrsArmedProfiler = true;
            Logger::Log::Info("RenderPipeline: dynamic resolution armed the render-graph GPU "
                              "profiler (per-pass timestamps) — it stays armed while DRS is on");
        }
    }

    // Resolve stats arrive FramesInFlight frames late and can legitimately come
    // back partial (query-pool cap, a slot whose fence has not returned). A
    // partial frame is not a cheap frame, so anything short of a clean resolve
    // is reported as "no sample" and the controller holds.
    const auto stats = frame.LastResolveStats();
    DynamicResolutionSample sample{};
    if (stats.ResolvedPasses > 0 && stats.InvalidQueryIndices == 0 && stats.ReadFailures == 0 &&
        stats.NonMonotonic == 0)
    {
        // Σ of the frame's distinct GPU measurements — an upper bound on GPU
        // busy time, and on backends that can only time whole command encoders
        // (TimestampSemantics::EncoderSpan) a loose one, because those spans
        // overlap each other. It is not the scalable portion either; the
        // controller's ratio law is damped precisely so that an unmeasured
        // fixed-cost component makes it under-correct (converge slowly) rather
        // than overshoot. Splitting the sum is a follow-up; see
        // temporal-upscaling-design.html §8d.
        sample.GpuMs = static_cast<float>(stats.DistinctSpanGpuMs);
        sample.Valid = true;
    }
    // Applied, not requested (see m_DrsLastAppliedScale). The value is from the
    // previous Declare while the cost sample is a few frames older still, but
    // the controller only records a response observation after a position has
    // been held for its settle time — several times the measurement latency —
    // so both refer to the same steady extent by construction.
    sample.AppliedScale = m_DrsLastAppliedScale;
    sample.ScaleIsConnected = m_DrsAnyViewEligible;
    // Keyed on the spine's app-frame epoch, NOT frame.FrameIndex(): every
    // window's spine declares on the main RenderServices with its own
    // per-window RGFrame counter, and those counters neither collide nor stay
    // aligned — the epoch is the only app-frame-global key, so N windows
    // collapse to one controller tick (the first declarer of the epoch wins).
    m_Rs.UpdateDynamicResolution(sample, deltaTimeSeconds, m_Rs.Spine().WorldFrameEpoch());
}

void RenderPipelineInstance::Declare(Rendering::RenderGraph::RGFrame& frame,
                                     std::span<const ViewTargetsRG> targets,
                                     std::span<const Rendering::ViewDesc> views,
                                     float deltaTimeSeconds)
{
    if (m_BlueprintDirty)
        EnsureNodeInstances();

    m_FrameResources.BeginFrame(frame);
    m_DeferredDeclares.clear(); // stale late declares die with their epoch

    TickDynamicResolution(frame, deltaTimeSeconds);
    m_DrsAnyViewEligible = false; // recomputed by the per-view pre-pass below

    auto findView = [&](Rendering::ViewId id) -> const Rendering::ViewDesc*
    {
        for (const auto& v : views)
            if (v.id == id)
                return &v;
        return nullptr;
    };

    const RenderScaleSplitNodes splitNodes = FindRenderScaleSplitNodes();

    // Per-view pre-pass: seed the View.* blackboard slots from the caller's
    // VALUES and apply the MSAA-off color collapse (when the color target is
    // single-sample the scene must render straight into the pipeline's
    // FX-sampled resolve target — there is no MSAA color to resolve). Descs
    // are read fresh from the frame, so the old in-place-MSAA-change
    // detection has nothing to detect.
    for (const auto& vt : targets)
    {
        const Rendering::ViewDesc* v = findView(vt.View);
        if (!v || !v->RenderedByPipeline())
            continue;
        if (v->worldPaused)
        {
            DeclarePausedView(frame, *v, vt);
            continue;
        }

        const ViewRasterExtent extent = FindViewRasterExtent(frame, *v, vt, splitNodes);
        const uint32_t samples = extent.Samples;
        uint32_t width = extent.Width;
        uint32_t height = extent.Height;
        const uint32_t outputWidth = width;
        const uint32_t outputHeight = height;
        Rendering::RenderGraph::RGTexture splitInternalDepth{};
        Rendering::RenderGraph::RGTexture splitInternalColor{};
        const bool multisampled = samples > 1;
        m_DrsAnyViewEligible = m_DrsAnyViewEligible || extent.SplitEligible;
        // Say once, with the missing part, why a multisampled view ignores its render scale.
        if (extent.MsaaBlocker && extent.CrossingAvailable && !m_WarnedRenderScaleUnderMsaa &&
            std::abs(extent.Scale - 1.0f) > kRenderScaleNativeEpsilon)
        {
            m_WarnedRenderScaleUnderMsaa = true;
            Logger::Log::Warning(
                "RenderPipeline: view {} asks for render scale {:.2f} with {}x MSAA, but {}, so it draws at "
                "full resolution. Give the view a single-sample resolve target and the pipeline an "
                "enabled DepthResolve node.",
                static_cast<uint32_t>(v->id), extent.Scale, samples, extent.MsaaBlocker);
        }
        if (extent.SplitWidth != width || extent.SplitHeight != height)
        {
            // The RG desc carries no usage: the caller's depth is DepthStencil|ShaderResource
            // everywhere the pipeline runs, and a multisampled color is resolved by a shader.
            splitInternalDepth = ImportRenderScaleSplitTarget(
                frame, v->id, "Depth", frame.Graph().ResourceDesc(vt.Depth.Id),
                static_cast<uint32_t>(Rendering::TextureUsage::DepthStencil) |
                    static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource),
                samples, extent.SplitWidth, extent.SplitHeight);
            if (multisampled && splitInternalDepth.IsValid())
                splitInternalColor = ImportRenderScaleSplitTarget(
                    frame, v->id, "ColorMsaa", frame.Graph().ResourceDesc(vt.Color.Id),
                    static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
                        static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource),
                    samples, extent.SplitWidth, extent.SplitHeight);
            // Import failure: no split this frame, at the native extent.
            if (multisampled && !splitInternalColor.IsValid())
                splitInternalDepth = {};
            if (splitInternalDepth.IsValid())
            {
                width = extent.SplitWidth;
                height = extent.SplitHeight;
            }
        }

        Rendering::RenderGraph::RGTexture resolveTarget{};
        if (!m_Blueprint.worldColorResolveTargetRef.empty())
        {
            // Through the blackboard resolver: the materializer creates the
            // pool-backed SceneColor on first request, sized to this view.
            ViewDeclare probe(frame, *v, m_Rs, "");
            probe.m_Instance = this;
            probe.RenderWidth = width;
            probe.RenderHeight = height;
            probe.OutputWidth = outputWidth;
            probe.OutputHeight = outputHeight;
            resolveTarget = probe.ResolveTexture(m_Blueprint.worldColorResolveTargetRef);
        }

        Rendering::RenderGraph::RGTexture color = splitInternalColor.IsValid() ? splitInternalColor : vt.Color;
        if (resolveTarget.IsValid() && vt.Color.IsValid() && resolveTarget.Id != vt.Color.Id &&
            !multisampled &&
            frame.Graph().ResourceDesc(resolveTarget.Id).SampleCount <= 1)
            color = resolveTarget; // MSAA-off collapse, pre-applied (a
                                   // multisampled resolve target is a blueprint
                                   // mis-spec — stay uncollapsed so the world
                                   // arm's own sample-count guard rejects it)
        const Rendering::RenderGraph::RGTexture resolve =
            resolveTarget.IsValid() ? resolveTarget
                                    : (vt.Resolve.IsValid() ? vt.Resolve : vt.Color);

        const bool splitActive = splitInternalDepth.IsValid();
        const Rendering::RenderGraph::RGTexture rasterDepth =
            splitActive ? splitInternalDepth : vt.Depth;
        m_FrameResources.Textures[{v->id, Names::View::Color}] = color;
        // The view's colour target is scene colour: the sky and the world write it at P.
        if (color.IsValid())
            m_FrameResources.PreExposedTextures[color.Id] = v->id;
        m_FrameResources.Textures[{v->id, Names::View::Depth}] = rasterDepth;
        m_FrameResources.Textures[{v->id, Names::View::Resolve}] = resolve;
        if (samples > 1 && m_Rs.GetEffectivePostProcessSettings(v->id, v->worldId).IsBloomActive())
            DeclareBloomSampleDelta(frame, v->id, resolve);
        m_FrameResources.Textures[{v->id, Names::View::DepthResolved}] = rasterDepth;
        if (splitActive)
        {
            // The caller's display-res targets, for the crossing node's upscale
            // point (output color republishes View.Resolve; output depth
            // receives the upsample the editor overlays depth-test against).
            // Their presence IS the "split is active" signal every crossing
            // node keys on. Under MSAA the output color is the caller's
            // single-sample resolve: the post chain writes it.
            m_FrameResources.Textures[{v->id, Names::View::OutputColor}] = multisampled ? vt.Resolve : vt.Color;
            m_FrameResources.Textures[{v->id, Names::View::OutputDepth}] = vt.Depth;
        }
        auto& info = m_FrameResources.Views[v->id];
        info.ResolveFromPipeline = resolveTarget.IsValid();
        info.RenderWidth = width;
        info.RenderHeight = height;
        info.OutputWidth = outputWidth;
        info.OutputHeight = outputHeight;

        // The scale the renderer ACTUALLY used, after the even-pixel snap and
        // the TAA-active gate. Dynamic resolution fits its response model
        // against this, never against the requested value: the requested scale
        // is not always the applied one, and a model fitted to a number the
        // renderer never used integrates error against a fiction.
        if (outputWidth > 0)
            m_DrsLastAppliedScale =
                static_cast<float>(width) / static_cast<float>(outputWidth);
    }

    PipelineDeclareContext ctx{m_Rs, frame, views, deltaTimeSeconds};

    // Whether this pipeline draws transmissive glass in a dedicated pass. If not, the world pass
    // must NOT peel glass (it would vanish). Cheap scan of the node table (a handful of nodes).
    bool hasTransmissivePass = false;
    for (const auto& [id, node] : m_Nodes)
        if (node && std::strcmp(node->GetTypeName(), "TransmissiveRender") == 0)
        {
            hasTransmissivePass = true;
            break;
        }

    // Whether this pipeline drains sorted transparents in a dedicated node. If not, the world pass
    // must NOT build the sorted set (the opaque peel would drop order-dependent Blend and nothing
    // would draw it → vanish). Same cheap node-table scan as the transmissive gate above.
    bool hasSortedTransparentPass = false;
    for (const auto& [id, node] : m_Nodes)
        if (node && std::strcmp(node->GetTypeName(), "SortedTransparent") == 0)
        {
            hasSortedTransparentPass = true;
            break;
        }

    // Node loop: blueprint array order IS the declaration order IS the
    // ordering (per-cell hazard tracking derives the edges; phase is only the
    // tiebreak a node forwards to AddPass). No tags, no RunAfterTag, no
    // usedThisBuild bookkeeping.
    for (uint32_t nodeIdx = 0; nodeIdx < static_cast<uint32_t>(m_Blueprint.passes.size());
         ++nodeIdx)
    {
        const auto& nb = m_Blueprint.passes[nodeIdx];
        if (!nb.enabled)
            continue;
        auto it = m_Nodes.find(nb.id);
        if (it == m_Nodes.end() || !it->second)
            continue;

        it->second->Declare(*this, ctx);

        if (!nb.perView)
            continue;
        for (const auto& vt : targets)
        {
            const Rendering::ViewDesc* v = findView(vt.View);
            if (!v || !v->RenderedByPipeline() || (v->worldPaused && !it->second->DeclaresForPausedView()))
                continue;
            ViewDeclare d(frame, *v, m_Rs, nb.id.c_str());
            d.m_Instance = this;
            d.m_NodeIdx = nodeIdx;
            d.PipelineHasTransmissivePass = hasTransmissivePass;
            d.PipelineHasSortedTransparentPass = hasSortedTransparentPass;
            d.ViewColor = TableTexture(v->id, Names::View::Color);
            d.ViewDepth = TableTexture(v->id, Names::View::Depth);
            d.ViewResolve = TableTexture(v->id, Names::View::Resolve);
            d.ViewBloomSampleDelta = TableTexture(v->id, Names::View::BloomSampleDelta);
            d.ViewDepthResolved = TableTexture(v->id, Names::View::DepthResolved);
            d.ViewOccluderDepthResolved = TableTexture(v->id, Names::View::OccluderDepthResolved);
            if (!d.ViewOccluderDepthResolved.IsValid())
                d.ViewOccluderDepthResolved = d.ViewDepthResolved;
            d.ViewOutputColor = TableTexture(v->id, Names::View::OutputColor);
            d.ViewOutputDepth = TableTexture(v->id, Names::View::OutputDepth);
            if (auto vi = m_FrameResources.Views.find(v->id); vi != m_FrameResources.Views.end())
            {
                d.ResolveFromPipeline = vi->second.ResolveFromPipeline;
                d.RenderWidth = vi->second.RenderWidth;
                d.RenderHeight = vi->second.RenderHeight;
                d.OutputWidth = vi->second.OutputWidth;
                d.OutputHeight = vi->second.OutputHeight;
            }
            it->second->DeclareForView(d);
        }
    }

    // Drain late declares (terminal overlays): every node has declared and
    // the blackboard holds the chain's final outputs; the spine's MarkOutput
    // runs after Declare returns. Index-based: a deferred fn may DeferDeclare
    // again (the vector can grow mid-drain; range-for would be UB).
    for (size_t i = 0; i < m_DeferredDeclares.size(); ++i)
        m_DeferredDeclares[i](frame, *this);
    m_DeferredDeclares.clear();
}

const RenderPipelineInstance::ResourceDescription*
RenderPipelineInstance::FindResourceDescription(std::string_view name) const
{
    const auto found = m_ResourceDescriptions.find(name);
    return found == m_ResourceDescriptions.end() ? nullptr : found->second.get();
}

void RenderPipelineInstance::DeclareBloomSampleDelta(Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId,
                                                     Rendering::RenderGraph::RGTexture resolve)
{
    if (!resolve.IsValid())
        return;
    const Rendering::RenderGraph::RGResourceDesc& resolveDesc = frame.Graph().ResourceDesc(resolve.Id);
    if (resolveDesc.SampleCount != 1)
        return;
    Rendering::TextureDesc td{};
    td.width = resolveDesc.Width;
    td.height = resolveDesc.Height;
    td.depth = 1;
    td.mipLevels = 1;
    td.arrayLayers = 1;
    td.sampleCount = 1;
    // The scene color's format: the delta is signed and has the HDR range of the samples.
    td.format = resolveDesc.Format;
    td.usage = static_cast<uint32_t>(Rendering::TextureUsage::RenderTarget) |
               static_cast<uint32_t>(Rendering::TextureUsage::ShaderResource);
    const std::string name = "Pipeline.View" + std::to_string(static_cast<uint32_t>(viewId)) + ".BloomSampleDelta";
    td.debugName = name.c_str();
    // Pool-persistent: a FullscreenShader input binds its physical at declaration, before a transient exists.
    const Rendering::RenderGraph::RGTexture delta = frame.ImportPersistentTexture(name.c_str(), td);
    if (!delta.IsValid())
        return;
    m_FrameResources.Textures[{viewId, Names::View::BloomSampleDelta}] = delta;
    m_FrameResources.PreExposedTextures[delta.Id] = viewId;
}

Rendering::RenderGraph::RGTexture RenderPipelineInstance::MaterializeTexture(
    Rendering::RenderGraph::RGFrame& frame, Rendering::ViewId viewId, std::string_view name,
    uint32_t renderW, uint32_t renderH, uint32_t outputW, uint32_t outputH)
{
    const auto* resource = FindResourceDescription(name);
    if (!resource || resource->Type != ResourceDescription::Kind::Texture)
        return {};
    const bool perView = resource->PerView;
    const Rendering::ViewId scopeView = perView ? viewId : 0;

    // First-import-wins: never two descriptions for one pool name in a frame.
    if (auto it = m_FrameResources.Textures.find({scopeView, name});
        it != m_FrameResources.Textures.end())
        return it->second;

    if (resource->OutputExtent && outputW > 0 && outputH > 0)
    {
        renderW = outputW;
        renderH = outputH;
    }
    if (!resource->AbsoluteExtent && (renderW == 0 || renderH == 0))
        return {}; // relative extent needs the current view's dimensions

    Rendering::TextureDesc td = resource->Texture;
    if (!resource->AbsoluteExtent)
    {
        td.width = std::max<uint32_t>(1u, (uint32_t)std::floor((float)renderW * resource->ScaleX));
        td.height = std::max<uint32_t>(1u, (uint32_t)std::floor((float)renderH * resource->ScaleY));
    }

    const std::string poolName = PoolResourceName(m_Blueprint.pipelineName, perView, viewId, name);
    td.debugName = poolName.c_str();

    const Rendering::RenderGraph::RGTexture t = frame.ImportPersistentTexture(poolName.c_str(), td);
    if (t.IsValid())
    {
        m_FrameResources.Textures[{scopeView, name}] = t;
        if (resource->PreExposed && perView)
            m_FrameResources.PreExposedTextures[t.Id] = viewId;
    }
    return t;
}

PipelineBufferBindingRG RenderPipelineInstance::MaterializeBuffer(Rendering::RenderGraph::RGFrame& frame,
                                                                  Rendering::ViewId viewId,
                                                                  std::string_view name,
                                                                  uint32_t renderW,
                                                                  uint32_t renderH)
{
    const auto* resource = FindResourceDescription(name);
    if (!resource || resource->Type != ResourceDescription::Kind::Buffer)
        return {};
    const auto& jr = resource->Buffer;
    const bool perView = resource->PerView;
    const Rendering::ViewId scopeView = perView ? viewId : 0;

    if (auto it = m_FrameResources.Buffers.find({scopeView, name});
        it != m_FrameResources.Buffers.end())
        return it->second;

    // Memory class first: Upload buffers dissolve into AllocUpload at their
    // owning node — a resolve miss here means the owner hasn't declared
    // (consumers treat that as absent, exactly like a missing publication).
    Rendering::BufferMemoryUsage mem = Rendering::BufferMemoryUsage::DeviceLocal;
    {
        std::string memStr;
        if (JsonTryGetString(jr, "memoryUsage", memStr))
        {
            if (auto m = BufferMemoryUsageTable().Lookup(memStr))
            {
                if (*m == Rendering::BufferMemoryUsage::Upload)
                    return {};
                if (*m == Rendering::BufferMemoryUsage::Readback)
                    mem = Rendering::BufferMemoryUsage::Readback;
                // Auto / DeviceLocal keep the DeviceLocal default (unchanged).
            }
            else
            {
                WarnUnknownEnumOnce("BufferMemoryUsage", memStr,
                                    BufferMemoryUsageTable().ValidNames());
            }
        }
        else
        {
            // Old-path default: ConstantBuffer-only usage = Upload.
            bool cbOnly = true;
            if (jr.contains("usage") && jr["usage"].is_array())
            {
                for (const auto& u : jr["usage"])
                {
                    if (!u.is_string())
                        continue;
                    const std::string us = ToLowerAscii(u.get<std::string>());
                    if (!(us == "constantbuffer" || us == "cb" || us == "cbv"))
                        cbOnly = false;
                }
            }
            if (cbOnly)
                return {};
        }
    }

    // Size: literal or expression against THIS view's render extent — the
    // same numbers dispatch sizing uses (cluster-grid agreement invariant).
    size_t sizeBytes = 0;
    if (jr.contains("sizeBytes") && jr["sizeBytes"].is_number_integer())
    {
        const int64_t s = jr["sizeBytes"].get<int64_t>();
        sizeBytes = s > 0 ? (size_t)s : 0;
    }
    else if (jr.contains("size") && jr["size"].is_object())
    {
        const auto& sz = jr["size"];
        if (sz.contains("bytes") && sz["bytes"].is_number_integer())
        {
            const int64_t s = sz["bytes"].get<int64_t>();
            sizeBytes = s > 0 ? (size_t)s : 0;
        }
        else
        {
            std::string expr;
            if (JsonTryGetString(sz, "expression", expr) && !expr.empty())
            {
                if (renderW == 0 || renderH == 0)
                    return {};
                // Blueprint constants first, then this view's extent, then the
                // resource-local variables (which may reference either and, on a
                // name clash, shadow the constant).
                std::unordered_map<std::string, double> vars;
                for (const auto& kv : m_Blueprint.constants)
                    vars[kv.first] = kv.second;
                vars["renderWidth"] = (double)renderW;
                vars["renderHeight"] = (double)renderH;
                std::string err;
                if (sz.contains("variables"))
                    (void)Expr::BuildVariables(sz["variables"], vars, &err);
                double v = 0.0;
                if (Expr::EvalExpression(expr, vars, v, &err) && v > 0.0)
                    sizeBytes = (size_t)std::llround(v);
            }
        }
    }
    if (sizeBytes == 0)
        return {};

    uint32_t usage = static_cast<uint32_t>(Rendering::BufferUsage::Storage);
    if (jr.contains("usage") && jr["usage"].is_array())
    {
        usage = 0;
        for (const auto& u : jr["usage"])
        {
            if (!u.is_string())
                continue;
            const std::string us = u.get<std::string>();
            if (auto f = BufferUsageTable().Lookup(us))
                usage |= static_cast<uint32_t>(*f);
            else
                WarnUnknownEnumOnce("BufferUsage", us, BufferUsageTable().ValidNames());
        }
    }

    // zeroOnCreate: history-class buffers (read before first write, contents
    // trusted across frames) must not observe recycled pool garbage — the
    // creation frame schedules a declared zero-fill before any consumer.
    bool zeroOnCreate = false;
    if (jr.contains("zeroOnCreate") && jr["zeroOnCreate"].is_boolean())
        zeroOnCreate = jr["zeroOnCreate"].get<bool>();
    if (zeroOnCreate)
        usage |= static_cast<uint32_t>(Rendering::BufferUsage::TransferDst);

    Rendering::BufferDesc bd{};
    bd.size = sizeBytes;
    bd.usage = usage;
    bd.memoryUsage = mem;

    const std::string poolName = PoolResourceName(m_Blueprint.pipelineName, perView, viewId, name);

    // perFrameCopies: a small history buffer one queue writes and another reads, kept as one copy
    // per frame in flight, so a frame's write never waits for an earlier frame's reads.
    if (jr.contains("perFrameCopies") && jr["perFrameCopies"].is_boolean() && jr["perFrameCopies"].get<bool>())
    {
        const PipelineBufferBindingRG binding = MaterializeBufferCopies(frame, name, poolName, bd);
        if (binding.IsValid())
            m_FrameResources.Buffers[{scopeView, name}] = binding;
        return binding;
    }

    bd.debugName = poolName.c_str();
    bool needsZeroInit = false;
    const Rendering::RenderGraph::RGBuffer b =
        frame.ImportPersistentBuffer(poolName.c_str(), bd, &needsZeroInit);
    if (!b.IsValid())
        return {};
    if (zeroOnCreate && needsZeroInit)
        frame.AddBufferZeroInit(b, std::string("ZeroInit.").append(name).c_str());
    PipelineBufferBindingRG binding{};
    binding.Buffer = frame.PhysicalBuffer(b);
    binding.Offset = 0;
    binding.Size = sizeBytes;
    binding.Graph = b;
    m_FrameResources.Buffers[{scopeView, name}] = binding;
    return binding;
}

PipelineBufferBindingRG RenderPipelineInstance::MaterializeBufferCopies(Rendering::RenderGraph::RGFrame& frame,
                                                                        std::string_view name,
                                                                        const std::string& poolName,
                                                                        Rendering::BufferDesc desc)
{
    // The copy this frame writes was last used CopyCount frames of this view ago. The device
    // has finished that frame before this one records (IDevice::BeginFrame waits for it), so the
    // render graph's cross-frame ordering finds that work complete and adds no wait.
    BufferCopiesState& state = m_BufferCopies.GetOrInsert(poolName);
    if (state.CarryPassName.empty())
    {
        state.CopyCount = frame.Device()->GetFrameRingDepth();
        for (uint32_t copy = 0; copy < state.CopyCount; ++copy)
            state.CopyNames[copy] = poolName + ".Copy" + std::to_string(copy);
        state.CarryPassName = std::string("Carry.").append(name);
        state.ZeroInitPassName = std::string("ZeroInit.").append(name);
    }
    if (!state.HasFrame || state.Frame != frame.FrameIndex())
    {
        // Rotate once per frame; a frame declared again keeps its copies.
        state.Previous = state.Current;
        state.HasPrevious = state.HasFrame;
        state.Current = state.HasFrame ? (state.Current + 1u) % state.CopyCount : 0u;
        state.Frame = frame.FrameIndex();
        state.HasFrame = true;
    }

    desc.usage |= static_cast<uint32_t>(Rendering::BufferUsage::TransferSrc) |
                  static_cast<uint32_t>(Rendering::BufferUsage::TransferDst);
    const std::string& currentName = state.CopyNames[state.Current];
    desc.debugName = currentName.c_str();
    bool currentFresh = false;
    const Rendering::RenderGraph::RGBuffer current =
        frame.ImportPersistentBuffer(currentName.c_str(), desc, &currentFresh);
    if (!current.IsValid())
        return {};

    // The frame starts from the previous frame's contents. A copy that holds none (the first frame,
    // a device rebuild, an evicted copy) starts at zero, as a zeroOnCreate buffer does.
    Rendering::RenderGraph::RGBuffer previous{};
    if (state.HasPrevious)
    {
        const std::string& previousName = state.CopyNames[state.Previous];
        desc.debugName = previousName.c_str();
        bool previousFresh = false;
        previous = frame.ImportPersistentBuffer(previousName.c_str(), desc, &previousFresh);
        if (previousFresh)
            previous = {};
    }
    // A fresh copy is zero-filled even when the carry overwrites it: only a fill clears the pool's
    // fresh mark, and the carry skips a previous copy that is still marked fresh.
    if (currentFresh || !previous.IsValid())
        frame.AddBufferZeroInit(current, state.ZeroInitPassName.c_str());
    if (previous.IsValid())
        DeclareBufferCarry(frame, previous, current, desc.size, state.CarryPassName.c_str());

    PipelineBufferBindingRG binding{};
    binding.Buffer = frame.PhysicalBuffer(current);
    binding.Offset = 0;
    binding.Size = desc.size;
    binding.Graph = current;
    return binding;
}

void RenderPipelineInstance::DeclareBufferCarry(Rendering::RenderGraph::RGFrame& frame,
                                                Rendering::RenderGraph::RGBuffer previous,
                                                Rendering::RenderGraph::RGBuffer current, size_t sizeBytes,
                                                const char* passName)
{
    // On the compute queue, where the shipped first writer of such a history (the pre-exposure
    // snapshot) runs. A carry on the graphics queue would run after the previous frame's graphics
    // work and make that writer wait for it.
    frame.AddComputePass(
        passName, Rendering::PassPhase::kEarlySetup,
        [&](Rendering::RenderGraph::RGPassBuilder& p)
        {
            p.Read(previous, Rendering::RenderGraph::RGBufferRead::CopySrc);
            p.Write(current, Rendering::RenderGraph::RGBufferWrite::CopyDst);
        },
        [previous, current, sizeBytes](Rendering::RenderGraph::RGContext& ctx)
        {
            const Rendering::BufferHandle src = ctx.GetBuffer(previous);
            const Rendering::BufferHandle dst = ctx.GetBuffer(current);
            if (ctx.Cmd != nullptr && src.IsValid() && dst.IsValid())
                ctx.Cmd->CopyBuffer(src, dst, sizeBytes);
        });
}

Rendering::RenderGraph::RGTexture RenderPipelineInstance::GetOutputRG(Rendering::ViewId viewId,
                                                              const std::string& outputName) const
{
    auto it = m_Outputs.find(outputName);
    const std::string_view ref = (it != m_Outputs.end()) ? std::string_view(it->second) : Names::View::Resolve;
    return TableTexture(viewId, ref);
}

void RenderPipelineInstance::DeclarePausedView(Rendering::RenderGraph::RGFrame& frame,
                                               const Rendering::ViewDesc& view, const ViewTargetsRG& vt)
{
    const Rendering::RenderGraph::RGTexture target = vt.Resolve.IsValid() ? vt.Resolve : vt.Color;
    if (!target.IsValid())
        return;
    constexpr uint32_t kPausedViewNameSlot = 0xFFFFu; // no blueprint pass has this index
    auto [name, inserted] = m_PassNameCache.try_emplace(MakePassNameKey(kPausedViewNameSlot, view.id));
    if (inserted)
        name->second = "Pipeline." + m_Blueprint.pipelineName + ".WorldPaused.View" + std::to_string(view.id);
    const ColorLinear clear = view.targets.clearColorValue;
    frame.AddPass(name->second.c_str(), Rendering::PassPhase::kDefault,
        [&](Rendering::RenderGraph::RGPassBuilder& p)
        {
            Rendering::RenderGraph::RGAttachmentOps ops{};
            ops.Load = Rendering::RenderGraph::RGLoadOp::Clear;
            ops.Store = Rendering::RenderGraph::RGStoreOp::Store;
            ops.Clear.Color[0] = clear.r;
            ops.Clear.Color[1] = clear.g;
            ops.Clear.Color[2] = clear.b;
            ops.Clear.Color[3] = clear.a;
            p.AttachColor(0, target, ops);
        }, [](Rendering::RenderGraph::RGContext&) {});
    m_FrameResources.Textures[{view.id, Names::View::Color}] = target;
    m_FrameResources.Textures[{view.id, Names::View::Resolve}] = target;
    for (const auto& [output, ref] : m_Outputs)
        m_FrameResources.Textures[{view.id, ref}] = target;
    RecordPausedViewWarmKeys(frame, view, vt);
    RequestPausedViewPipelines(frame, view, vt);
}

void RenderPipelineInstance::RequestPausedViewPipelines(const Rendering::RenderGraph::RGFrame& frame,
                                                        const Rendering::ViewDesc& view, const ViewTargetsRG& vt)
{
    // The live frame renders at the split extent when the split's target imports succeed.
    const ViewRasterExtent extent = FindViewRasterExtent(frame, view, vt, FindRenderScaleSplitNodes());
    const PausedViewPrewarm prewarm(m_Rs, view, extent.SplitWidth, extent.SplitHeight, frame, vt, *this);
    for (const auto& nb : m_Blueprint.passes)
    {
        if (!nb.enabled || !nb.perView)
            continue;
        const auto it = m_Nodes.find(nb.id);
        if (it != m_Nodes.end() && it->second)
            it->second->RequestPausedViewPipelines(prewarm);
    }
}

RenderPipelineInstance::RenderScaleSplitNodes RenderPipelineInstance::FindRenderScaleSplitNodes() const
{
    RenderScaleSplitNodes nodes{};
    // m_Nodes also holds disabled passes; only a pass that Declare runs counts.
    for (const auto& pass : m_Blueprint.passes)
    {
        if (!pass.enabled)
            continue;
        const auto nodeIt = m_Nodes.find(pass.id);
        if (nodeIt == m_Nodes.end() || !nodeIt->second)
            continue;
        const char* typeName = nodeIt->second->GetTypeName();
        nodes.HasRenderScaleUpscale = nodes.HasRenderScaleUpscale || std::strcmp(typeName, "RenderScaleUpscale") == 0;
        nodes.HasDepthResolve = nodes.HasDepthResolve || std::strcmp(typeName, "DepthResolve") == 0;
    }
    return nodes;
}

RenderPipelineInstance::ViewRasterExtent
RenderPipelineInstance::FindViewRasterExtent(const Rendering::RenderGraph::RGFrame& frame, const Rendering::ViewDesc& view,
                                             const ViewTargetsRG& vt, const RenderScaleSplitNodes& splitNodes) const
{
    ViewRasterExtent extent{};
    if (vt.Color.IsValid())
    {
        const auto& cd = frame.Graph().ResourceDesc(vt.Color.Id);
        extent.Samples = cd.SampleCount > 0 ? cd.SampleCount : 1;
        extent.Width = cd.Width;
        extent.Height = cd.Height;
    }
    else if (vt.Depth.IsValid())
    {
        const auto& dd = frame.Graph().ResourceDesc(vt.Depth.Id);
        extent.Width = dd.Width;
        extent.Height = dd.Height;
    }
    extent.SplitWidth = extent.Width;
    extent.SplitHeight = extent.Height;

    // Internal-resolution split: an unletterboxed view at a render scale other than 1.0 draws (and, under MSAA,
    // resolves) at the internal extent. One pass crosses to the display extent: the TAA resolve or RenderScaleUpscale.
    // A letterboxed view does not split. At scale 1.0 the extent equals the unsplit one.
    extent.Scale = m_Rs.ResolveViewRenderScale(view.id);
    // Eligibility is everything EXCEPT the scale test: a view can be able to split and
    // sit at native. Dynamic resolution must know whether the lever is connected at all
    // (eligible) or only idle (eligible at 1.0), else it freezes at native.
    extent.CrossingAvailable =
        splitNodes.HasRenderScaleUpscale || m_Rs.Views().FindViewAntiAliasing(view.id) != nullptr;
    extent.MsaaBlocker = extent.Samples > 1 ? MultisampledSplitBlocker(frame, vt.Resolve, extent.Width, extent.Height,
                                                                       splitNodes.HasDepthResolve)
                                            : nullptr;
    extent.SplitEligible = extent.Width > 0 && extent.Height > 0 && vt.Depth.IsValid() &&
                           extent.MsaaBlocker == nullptr && !m_Blueprint.worldColorResolveTargetRef.empty() &&
                           extent.CrossingAvailable && !m_Rs.Views().GetViewLetterbox(view.id).active;
    // Both directions split: below 1.0 renders reduced and upscales, above 1.0 renders
    // supersampled and the crossing filters back down.
    if (!extent.SplitEligible || std::abs(extent.Scale - 1.0f) <= kRenderScaleNativeEpsilon)
        return extent;
    // Even-snapped floor: the half and quarter chains divide cleanly, and the extent is
    // stable for one display extent and scale.
    extent.SplitWidth = ScaledEvenExtent(extent.Width, extent.Scale);
    extent.SplitHeight = ScaledEvenExtent(extent.Height, extent.Scale);
    return extent;
}

uint32_t RenderPipelineInstance::ScaledEvenExtent(uint32_t extent, float scale)
{
    const uint32_t scaled = static_cast<uint32_t>(std::floor(static_cast<float>(extent) * scale)) & ~1u;
    return std::max(2u, scaled);
}

Rendering::RenderGraph::RGAttachmentKeyInput
RenderPipelineInstance::LiveColorTargetInput(const Rendering::RenderGraph::RGFrame& frame, const ViewTargetsRG& vt,
                                             std::string_view ref) const
{
    namespace RenderGraph = Rendering::RenderGraph;
    if (ref != Names::View::Color && ref != Names::View::Resolve)
        return DescribedColorInput(FindResourceDescription(ref));
    if (!vt.Color.IsValid())
        return {};
    const RenderGraph::RGResourceDesc& color = frame.Graph().ResourceDesc(vt.Color.Id);
    const RenderGraph::RGAttachmentKeyInput resolveTarget =
        DescribedColorInput(FindResourceDescription(m_Blueprint.worldColorResolveTargetRef));
    const bool hasResolveTarget = resolveTarget.Format != Rendering::TextureFormat{};
    if (ref == Names::View::Color && !(hasResolveTarget && color.SampleCount <= 1 && resolveTarget.SampleCount <= 1))
        return {0u, false, static_cast<Rendering::TextureFormat>(color.Format), std::max(color.SampleCount, 1u)};
    if (hasResolveTarget)
        return resolveTarget;
    const RenderGraph::RGResourceDesc& resolve = frame.Graph().ResourceDesc(vt.Resolve.IsValid() ? vt.Resolve.Id : vt.Color.Id);
    return {0u, false, static_cast<Rendering::TextureFormat>(resolve.Format), std::max(resolve.SampleCount, 1u)};
}

Rendering::RenderGraph::RGAttachmentKeyInput
RenderPipelineInstance::DescribedColorInput(const ResourceDescription* description)
{
    if (!description || description->Type != ResourceDescription::Kind::Texture)
        return {};
    return {0u, false, static_cast<Rendering::TextureFormat>(description->Texture.format),
            std::max(description->Texture.sampleCount, 1u)};
}

std::optional<Rendering::PipelineFormatKey> PausedViewPrewarm::SingleColorTargetKey(std::string_view ref) const
{
    const Rendering::RenderGraph::RGAttachmentKeyInput input = m_Instance.LiveColorTargetInput(m_Frame, m_Targets, ref);
    if (input.Format == Rendering::TextureFormat{})
        return std::nullopt;
    const IDevice* device = Services.GetDevice();
    return Rendering::RenderGraph::DeriveFormatKey(&input, 1u, device ? device->GetSwapchainTextureFormat()
                                                                      : Rendering::TextureFormat{});
}

std::optional<Rendering::PipelineFormatKey> PausedViewPrewarm::WorldColorTargetsKey() const
{
    return m_Instance.WorldColorTargetsFormatKey(m_Frame, m_Targets);
}

bool PausedViewPrewarm::ReservesOcclusionSliceOnceUnpaused() const
{
    // The terms of RenderServices::ScheduleViewCullingDispatches and of the slice layout in
    // GPUCullingPipeline::EndFrameRG, with the layers the view has once unpaused.
    const Rendering::GPUScene* scene = Services.GetGPUScene();
    if (!scene || !Services.GetGPUCullingPipeline() || scene->GetInstanceCount() == 0u)
        return false;
    if (View.cameraId == 0 || View.UnpausedRenderLayerMask() == Rendering::RenderLayerMask::None() ||
        !Services.Views().FindCameraData(View.cameraId))
        return false;
    return View.cullingStrategy && View.cullingStrategy->ReservesOcclusionSlice();
}

std::optional<Rendering::PipelineFormatKey>
RenderPipelineInstance::WorldColorTargetsFormatKey(const Rendering::RenderGraph::RGFrame& frame,
                                                   const ViewTargetsRG& vt) const
{
    namespace RenderGraph = Rendering::RenderGraph;
    if (!vt.Color.IsValid())
        return std::nullopt;
    // The attachments of WorldColorFormatKey. With MSAA off the world pass draws into the pipeline's
    // resolve target (the collapse in Declare).
    const RenderGraph::RGResourceDesc& color = frame.Graph().ResourceDesc(vt.Color.Id);
    std::array<RenderGraph::RGAttachmentKeyInput, 2> inputs{};
    uint32_t count = 0;
    inputs[count++] = {0u, false, static_cast<Rendering::TextureFormat>(color.Format), color.SampleCount};
    const ResourceDescription* resolve = FindResourceDescription(m_Blueprint.worldColorResolveTargetRef);
    if (resolve && resolve->Type == ResourceDescription::Kind::Texture && color.SampleCount <= 1 &&
        resolve->Texture.sampleCount <= 1)
        inputs[0] = {0u, false, static_cast<Rendering::TextureFormat>(resolve->Texture.format), 1u};
    if (vt.Depth.IsValid())
    {
        const RenderGraph::RGResourceDesc& depth = frame.Graph().ResourceDesc(vt.Depth.Id);
        inputs[count++] = {0u, true, static_cast<Rendering::TextureFormat>(depth.Format), depth.SampleCount};
    }
    // A formatless import has no key, as in WorldColorFormatKey.
    if (std::any_of(inputs.begin(), inputs.begin() + count,
                    [](const RenderGraph::RGAttachmentKeyInput& in) { return in.Format == Rendering::TextureFormat{}; }))
        return std::nullopt;
    return RenderGraph::DeriveFormatKey(inputs.data(), count, Rendering::TextureFormat{});
}

void RenderPipelineInstance::RecordPausedViewWarmKeys(Rendering::RenderGraph::RGFrame& frame,
                                                      const Rendering::ViewDesc& view, const ViewTargetsRG& vt)
{
    if (!m_WorldRenderPassKeywords || !vt.Color.IsValid())
        return;
    // A view that drew keeps its format key and the keywords that its world pass added (reflection,
    // ray-traced and screen-space shadow). The keywords of the view's state follow its settings.
    if (m_Rs.Views().GetViewWorldColorSampleCount(view.id) != 0)
    {
        auto* drawn = m_Rs.Views().FindPerView(view.id);
        if (drawn && drawn->DeclaredWorldPassKeywords)
            drawn->DeclaredWorldPassKeywords = Nodes::WorldPassViewKeywords(
                m_Rs, view, *drawn->DeclaredWorldPassKeywords & ~Nodes::kWorldPassViewStateKeywords,
                m_HeightFogParamsBeforeWorldRender);
        return;
    }
    const std::optional<Rendering::PipelineFormatKey> formats = WorldColorTargetsFormatKey(frame, vt);
    if (!formats)
        return;
    // The keys leave out what other passes add: the reflection slices and their keywords, the
    // ray-traced shadow keyword and the relief depth source. The screen-space shadow mask needs the
    // prepass depth, so it follows the shadow settings when a DepthPrepass runs ahead of the world pass.
    auto& perView = m_Rs.Views().PerView(view.id);
    perView.WorldColorFormatKey = *formats;
    Rendering::MaterialKeyword keywords = Nodes::WorldPassViewKeywords(m_Rs, view, *m_WorldRenderPassKeywords,
                                                                       m_HeightFogParamsBeforeWorldRender);
    if (m_DepthPrepassBeforeWorldRender &&
        TakesScreenSpaceShadowMask(m_Rs, view.worldId, m_Rs.GetWorldShadowSettings(view.worldId), false, keywords))
        keywords = keywords | Rendering::MaterialKeyword::ScreenSpaceShadows;
    perView.DeclaredWorldPassKeywords = keywords;
}

const PipelineFrameResources*
RenderPipelineInstance::FrameResourcesFor(const Rendering::RenderGraph::RGFrame* frame) const
{
    // (Frame, FrameIndex) — pointer equality alone validates a stale
    // incarnation when the frame was re-begun (passive RenderSingle) or a
    // new RGFrame landed at a recycled address.
    return (frame && m_FrameResources.Frame == frame &&
            m_FrameResources.FrameIndex == frame->FrameIndex())
               ? &m_FrameResources
               : nullptr;
}

Rendering::RenderGraph::RGTexture ViewDeclare::ResolveTexture(std::string_view ref) const
{
    if (ref == Names::View::Color)
        return ViewColor;
    if (ref == Names::View::Depth)
        return ViewDepth;
    if (ref == Names::View::Resolve)
        return ViewResolve;
    if (ref == Names::View::DepthResolved)
        return ViewDepthResolved;
    if (!m_Instance)
        return {};
    if (auto t = m_Instance->TableTexture(View.id, ref); t.IsValid())
        return t;
    return m_Instance->MaterializeTexture(Frame, View.id, ref, RenderWidth, RenderHeight,
                                          OutputWidth, OutputHeight);
}

PipelineBufferBindingRG ViewDeclare::ResolveBuffer(std::string_view ref) const
{
    if (!m_Instance)
        return {};
    if (auto b = m_Instance->TableBuffer(View.id, ref); b.IsValid())
        return b;
    return m_Instance->MaterializeBuffer(Frame, View.id, ref, RenderWidth, RenderHeight);
}

void ViewDeclare::PublishTexture(std::string_view name, Rendering::RenderGraph::RGTexture t)
{
    if (m_Instance)
        m_Instance->m_FrameResources.Textures[{View.id, name}] = t;
}

void ViewDeclare::MarkPreExposed(Rendering::RenderGraph::RGTexture t)
{
    if (m_Instance && t.IsValid())
        m_Instance->m_FrameResources.PreExposedTextures[t.Id] = View.id;
}

bool ViewDeclare::IsPreExposed(Rendering::RenderGraph::RGTexture t) const
{
    if (!m_Instance || !t.IsValid())
        return false;
    const auto it = m_Instance->m_FrameResources.PreExposedTextures.find(t.Id);
    return it != m_Instance->m_FrameResources.PreExposedTextures.end() && it->second == View.id;
}

void ViewDeclare::PublishBuffer(std::string_view name, const PipelineBufferBindingRG& b)
{
    if (m_Instance)
        m_Instance->m_FrameResources.Buffers[{View.id, name}] = b;
}

void ViewDeclare::DeferDeclare(
    std::function<void(Rendering::RenderGraph::RGFrame&, RenderPipelineInstance&)> fn) const
{
    if (m_Instance)
        m_Instance->DeferDeclare(std::move(fn));
}

const std::string& ViewDeclare::PassName(const char* suffix) const
{
    static const std::string kUnowned = "Pipeline.Unowned";
    if (!m_Instance)
        return kUnowned;
    return m_Instance->DeclarePassName(m_NodeIdx, View.id, suffix);
}

uint64_t RenderPipelineInstance::MakePassNameKey(uint32_t nodeIdx, uint32_t viewId, uint16_t suffixHash)
{
    assert(nodeIdx < (1u << 16) && "Node index exceeds 16-bit key field");
    return (static_cast<uint64_t>(nodeIdx) << 48)
         | (static_cast<uint64_t>(suffixHash) << 32)
         | viewId;
}

const std::string& RenderPipelineInstance::ResolveSuffixedPassName(
    uint32_t nodeIdx, uint32_t viewId, const std::string& baseName, const char* suffix)
{
    // Hash the suffix to a 16-bit key. Known suffixes are compile-time literals
    // (e.g. "Upload", "Cascade0"), so collision risk is negligible.
    const size_t len = std::strlen(suffix);
    const uint16_t suffixHash = static_cast<uint16_t>(Fnv1a64(suffix, len) & 0xFFFF);
    const uint64_t key = MakePassNameKey(nodeIdx, viewId, suffixHash);

    auto [it, inserted] = m_PassNameCache.try_emplace(key);
    if (inserted)
        it->second = baseName + "." + suffix;
    return it->second;
}

} // namespace GameEngine::Engine::Renderer::Pipeline
