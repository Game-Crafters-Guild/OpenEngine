#include "Assets/AssetsFolderDropRegistry.h"

#include "UI/Interaction/DropTarget.h"

namespace GameEngine::Editor
{

AssetsFolderDropRegistry& AssetsFolderDropRegistry::Get()
{
    static AssetsFolderDropRegistry s_Registry;
    return s_Registry;
}

void AssetsFolderDropRegistry::Register(UI::Interaction::PayloadTypeId payloadType, AssetsFolderDropAcceptor acceptor)
{
    m_Acceptors.InsertOrAssign(payloadType, acceptor);
}

bool AssetsFolderDropRegistry::Accepts(UI::Interaction::PayloadTypeId payloadType) const
{
    return m_Acceptors.Find(payloadType) != nullptr;
}

UI::Interaction::DropFeedback AssetsFolderDropRegistry::CanDrop(const AssetsFolderDrop& drop) const
{
    const AssetsFolderDropAcceptor* acceptor = m_Acceptors.Find(drop.Request.payload.TypeId);
    if (!acceptor || !acceptor->CanDrop)
        return {false, "The Assets panel does not take this item."};
    return acceptor->CanDrop(drop);
}

std::vector<std::filesystem::path> AssetsFolderDropRegistry::PerformDrop(const AssetsFolderDrop& drop) const
{
    const AssetsFolderDropAcceptor* acceptor = m_Acceptors.Find(drop.Request.payload.TypeId);
    if (!acceptor || !acceptor->PerformDrop)
        return {};
    return acceptor->PerformDrop(drop);
}

} // namespace GameEngine::Editor
