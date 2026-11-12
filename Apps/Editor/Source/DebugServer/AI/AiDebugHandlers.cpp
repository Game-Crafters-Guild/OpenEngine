#include "DebugServer/AI/AiDebugHandlers.h"

#include "AIECS/Components/AiPathTrace.h"
#include "AIECS/Components/AiTrace.h"
#include "Core/Engine.h"
#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <string>

namespace GameEngine
{

namespace
{

using json = nlohmann::json;

static_assert(Components::AiPathTrace::kCapacity == Components::AiTrace::kCapacity,
              "get_ai_trace reads one ticks limit for both traces");

json HandleAiTrace(const EditorDebugServer::RequestContext& ctx)
{
    if (!ctx.params.contains("entityId") || !ctx.params["entityId"].is_number_unsigned())
        return Editor::RefuseRequest("Pass entityId: the id of an entity with an AiTrace or AiPathTrace component");
    const std::optional<uint32> ticks = Editor::ReadAiTraceTicks(ctx.params);
    if (!ticks)
        return Editor::RefuseRequest("ticks must be a whole number from 1 to " +
                                     std::to_string(Components::AiTrace::kCapacity));
    ECS::World* world = EngineCore::GetInstance().GetPrimaryWorld();
    if (world == nullptr)
        return Editor::RefuseRequest("No world is loaded: open a scene first");
    const ECS::EntityHandle entity(ctx.params["entityId"].get<uint32>());
    const Components::AiTrace* trace = world->IsValid(entity) ? world->GetComponent<Components::AiTrace>(entity) : nullptr;
    const Components::AiPathTrace* pathTrace =
        world->IsValid(entity) ? world->GetComponent<Components::AiPathTrace>(entity) : nullptr;
    if (trace == nullptr && pathTrace == nullptr)
        return Editor::RefuseRequest("Entity " + std::to_string(entity.id) +
                                     " has no AiTrace or AiPathTrace: add AiTrace to a utility agent or AiPathTrace to "
                                     "a state-tree agent to record its decisions");
    json reply = json::object();
    if (trace != nullptr)
        reply.update(Editor::BuildAiTraceReport(*trace, *ticks));
    if (pathTrace != nullptr)
        reply.update(Editor::BuildAiPathTraceReport(*pathTrace, *ticks));
    reply["entityId"] = entity.id;
    return reply;
}

} // namespace

void RegisterAiDebugHandlers(EditorDebugServer& server)
{
    // get_ai_trace: one agent's last decisions. Params: entityId (an entity with AiTrace or AiPathTrace) and
    // ticks (the newest records to return, 1 to 64, default 64). Reply: entityId, recorded and records for
    // AiTrace (BuildAiTraceReport), pathRecorded and pathRecords for AiPathTrace (BuildAiPathTraceReport).
    server.RegisterHandler("get_ai_trace", HandleAiTrace);
}

} // namespace GameEngine
