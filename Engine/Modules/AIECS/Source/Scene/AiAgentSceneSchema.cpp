#include "AIECS/AiRegistry.h"
#include "AIECS/Components/AiAgent.h"
#include "AssetCore/GUID.h"
#include "ECS/ECSTemplates.h"
#include "Logger/Logger.h"
#include "Scene/AiSceneText.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scene/SceneValue.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine::AIECS
{
namespace
{

template <typename T>
bool ReadInteger(std::string_view value, T& out, std::string* outError)
{
    if (Scene::ParseIntegerToken(value, out) == Scene::IntegerTokenResult::Ok)
        return true;
    if (outError)
        *outError = "expected a whole number from 0 to " + std::to_string(std::numeric_limits<T>::max());
    return false;
}

bool ReadFloat(std::string_view value, float32& out, std::string* outError)
{
    if (Scene::ParseFloat(value, out))
        return true;
    if (outError)
        *outError = "expected a number";
    return false;
}

bool ReadBool(std::string_view value, bool& out, std::string* outError)
{
    Scene::SceneValue parsed;
    if (!Scene::ParseValue(value, parsed, outError) || parsed.Kind != Scene::SceneValueKind::Bool)
    {
        if (outError)
            *outError = "expected true or false";
        return false;
    }
    out = parsed.BoolValue;
    return true;
}

// The program whose stable id the quoted GUID names.
bool ReadProgram(std::string_view value, uint16& out, std::string* outError)
{
    std::string text;
    if (!Scene::ParseQuotedString(value, text) || !Scene::IsGuidText(text))
    {
        if (outError)
            *outError = "expected the quoted GUID of an option-set asset";
        return false;
    }
    out = FindProgramByStableId(AiRegistry::Get(), GUID(text));
    if (out != Components::AiAgent::kNoProgram)
        return true;
    if (outError)
        *outError = "no program has the stable id " + text +
                    ". Compile the option set with AiRegistry::CompileProgram before the scene loads";
    return false;
}

bool ReadPath(std::string_view value, Components::AiAgent& agent, std::string* outError)
{
    Scene::SceneValue list;
    if (!Scene::ParseValue(value, list, outError))
        return false;
    const bool fits = list.Kind == Scene::SceneValueKind::Array && list.Items.size() <= Components::AiAgent::kMaxPathDepth;
    for (size_t level = 0; fits && level < list.Items.size(); ++level)
    {
        const Scene::SceneValue& item = list.Items[level];
        if (item.Kind != Scene::SceneValueKind::Int || item.IntValue < 0 || item.IntValue >= Components::AiAgent::kNoState)
        {
            if (outError)
                *outError = "level " + std::to_string(level) + " of the active path is not a state index";
            return false;
        }
        agent.ActivePath[level] = static_cast<uint16>(item.IntValue);
    }
    if (!fits)
    {
        if (outError)
            *outError = "expected a list of at most " + std::to_string(Components::AiAgent::kMaxPathDepth) + " state indices";
        return false;
    }
    for (size_t level = list.Items.size(); level < Components::AiAgent::kMaxPathDepth; ++level)
        agent.ActivePath[level] = Components::AiAgent::kNoState;
    agent.PathDepth = static_cast<uint8>(list.Items.size());
    return true;
}

// Empties an active path that names an option the path's program does not have, so the agent
// chooses again at its next decision.
void KeepPathInProgram(Components::AiAgent& agent)
{
    const AiProgram* program = AiRegistry::Get().FindProgram(agent.PathProgram);
    if (program == nullptr || agent.PathDepth == 0 || agent.ActivePath[0] < program->Options.Options.size())
        return;
    agent.ActivePath[0] = Components::AiAgent::kNoState;
    agent.PathDepth = 0;
}

std::string FormatPath(const Components::AiAgent& agent)
{
    std::string text = "[";
    for (uint8 level = 0; level < agent.PathDepth; ++level)
        text += (level > 0 ? ", " : "") + std::to_string(agent.ActivePath[level]);
    return text + "]";
}

// The scene schema of AiAgent. The program and the path's program save as their stable ids
// (AiProgram::StableId), and the active path as option indices of the path's program. The absolute
// ticks are correct after a load when the save keeps the simulation tick
// (SceneWriteOptions::writeSimulationTick).
class AiAgentSceneSchema final : public Scene::ISceneComponentSchema
{
  public:
    std::string_view GetComponentName() const override { return "AiAgent"; }

    void Serialize(const ECS::World& world, ECS::EntityHandle entity, const Scene::SceneWriteContext&,
                   std::vector<std::string>& outLines) const override
    {
        const Components::AiAgent* agent = world.GetComponent<Components::AiAgent>(entity);
        if (agent == nullptr)
            return;
        const AiProgram* program = AiRegistry::Get().FindProgram(agent->Program);
        if (program != nullptr && !program->StableId.IsNull())
            outLines.push_back("AiAgent.program = " + Scene::FormatQuoted(program->StableId.ToString()));
        else if (program != nullptr)
            ReportProgramWithNoStableId(agent->Program);
        const AiProgram* pathProgram = AiRegistry::Get().FindProgram(agent->PathProgram);
        if (pathProgram != nullptr && !pathProgram->StableId.IsNull())
            outLines.push_back("AiAgent.pathProgram = " + Scene::FormatQuoted(pathProgram->StableId.ToString()));
        outLines.push_back("AiAgent.cadence = " + std::to_string(agent->Cadence));
        outLines.push_back("AiAgent.nextTick = " + std::to_string(agent->NextTick));
        outLines.push_back("AiAgent.enteredTick = " + std::to_string(agent->EnteredTick));
        outLines.push_back("AiAgent.activeScore = " + Scene::FormatFloat(agent->ActiveScore));
        outLines.push_back("AiAgent.drawSeed = " + std::to_string(agent->DrawSeed));
        outLines.push_back("AiAgent.activePath = " + FormatPath(*agent));
        outLines.push_back(std::string("AiAgent.scheduled = ") + (agent->Scheduled ? "true" : "false"));
    }

    bool ApplyProperty(ECS::World& world, ECS::EntityHandle entity, const Scene::SceneReadContext& ctx,
                       std::string_view property, std::string_view value, std::string* outError) const override
    {
        Components::AiAgent agent;
        if (const Components::AiAgent* existing = world.GetComponent<Components::AiAgent>(entity))
            agent = *existing;
        bool read = true;
        if (property == "program")
            read = ReadProgram(value, agent.Program, outError);
        else if (property == "pathprogram")
            read = ReadProgram(value, agent.PathProgram, outError);
        else if (property == "cadence")
            read = ReadInteger(value, agent.Cadence, outError);
        else if (property == "nexttick")
            read = ReadInteger(value, agent.NextTick, outError);
        else if (property == "enteredtick")
            read = ReadInteger(value, agent.EnteredTick, outError);
        else if (property == "activescore")
            read = ReadFloat(value, agent.ActiveScore, outError);
        else if (property == "drawseed")
            read = ReadInteger(value, agent.DrawSeed, outError);
        else if (property == "activepath")
            read = ReadPath(value, agent, outError);
        else if (property == "scheduled")
        {
            bool scheduled = false;
            read = ReadBool(value, scheduled, outError);
            agent.Scheduled = scheduled;
        }
        else
            return Scene::ReportUnknownField(ctx, GetComponentName(), property, outError);
        if (!read)
            return false;
        KeepPathInProgram(agent);
        if (Components::AiAgent* existing = world.GetComponentForWrite<Components::AiAgent>(entity))
            *existing = agent;
        else
            world.AddComponentImmediate(entity, agent);
        return true;
    }

    bool AddDefault(ECS::World& world, ECS::EntityHandle entity, std::string*) const override
    {
        if (world.GetComponent<Components::AiAgent>(entity) == nullptr)
            world.AddComponentImmediate(entity, Components::AiAgent{});
        return true;
    }

    bool Remove(ECS::World& world, ECS::EntityHandle entity) const override
    {
        world.RemoveComponentImmediate<Components::AiAgent>(entity);
        return true;
    }

  private:
    // Logs one warning for each program that a save cannot name.
    void ReportProgramWithNoStableId(uint16 program) const
    {
        std::scoped_lock lock(m_ReportedMutex);
        if (std::find(m_ReportedPrograms.begin(), m_ReportedPrograms.end(), program) != m_ReportedPrograms.end())
            return;
        m_ReportedPrograms.push_back(program);
        Logger::Log::Warning("AiAgent scene save: program {} has no stable id, so the save does not keep it, and its "
                             "agents do not decide after a load. Compile the program from an option-set asset with "
                             "AiRegistry::CompileProgram, or set AiProgram::StableId before AiRegistry::AddProgram.",
                             program);
    }

    mutable std::mutex m_ReportedMutex;
    mutable std::vector<uint16> m_ReportedPrograms;
};

} // namespace

GE_REGISTER_SCENE_SCHEMA(AiAgentSceneSchema)

} // namespace GameEngine::AIECS
