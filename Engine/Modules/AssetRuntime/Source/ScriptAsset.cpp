#include "Assets/ScriptAsset.h"

#include "AssetCore/SharedFileRead.h"

namespace GameEngine
{

bool ScriptAsset::Load()
{
    try
    {
        ByteBuffer bytes;
        if (!ReadFileBytesShared(GetPath(), bytes))
        {
            SetState(AssetState::Failed);
            return false;
        }

        return LoadFromData(bytes);
    }
    catch (...)
    {
        SetState(AssetState::Failed);
        return false;
    }
}

bool ScriptAsset::LoadFromData(std::span<const uint8> data)
{
    try
    {
        m_Text.assign(reinterpret_cast<const char*>(data.data()), data.size());
        SetState(AssetState::Loaded);
        return true;
    }
    catch (...)
    {
        SetState(AssetState::Failed);
        return false;
    }
}

void ScriptAsset::Unload()
{
    m_Text.clear();
    SetState(AssetState::Unloaded);
}

} // namespace GameEngine
