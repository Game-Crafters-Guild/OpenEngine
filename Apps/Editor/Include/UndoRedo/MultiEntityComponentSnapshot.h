#pragma once

#include "UndoRedo/History/HistoryComponentTargets.h"
#include "UndoRedo/UndoRedoService.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor::MultiEntityUndo
{

/// One component across a fixed selection, using the shared transactional snapshot format.
inline UndoRedoService::SnapshotTarget MakeComponentSnapshotTarget(
    std::function<ECS::World*()> resolveWorld, std::vector<ECS::EntityHandle> entities,
    ECS::ComponentTypeId typeId, EditorChangeNotifications* notifications, std::string debugLabel)
{
    auto targets = std::make_shared<HistoryComponentTargets>(std::move(resolveWorld), std::move(entities), typeId, notifications);
    UndoRedoService::SnapshotTarget target;
    target.debugLabel = std::move(debugLabel);
    target.Identity = targets->GetIdentity();
    target.Validate = [targets] { return targets->IsValid(); };
    target.Capture = [targets](auto& output) { return targets->Capture(output); };
    target.Apply = [targets](const auto& input) { return targets->Restore(input); };
    target.Notify = [targets](EditChangeKind kind) { targets->Notify(kind); };
    return target;
}

inline UndoRedoService::SnapshotTarget MakeComponentSnapshotTarget(
    ECS::World* world, std::vector<ECS::EntityHandle> entities, ECS::ComponentTypeId typeId,
    EditorChangeNotifications* notifications, std::string debugLabel)
{
    return MakeComponentSnapshotTarget([world] { return world; }, std::move(entities), typeId,
        notifications, std::move(debugLabel));
}

} // namespace GameEngine::Editor::MultiEntityUndo
