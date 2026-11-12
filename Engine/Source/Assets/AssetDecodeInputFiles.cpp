#include "Assets/AssetDecodeInputFiles.h"

#include "AssetCore/SharedFileRead.h"

#include <algorithm>
#include <utility>

namespace GameEngine {

namespace
{
    // Single definition in Engine.dll, as the decode cancel flag: every host shares the one
    // Engine module, so the files are visible to the whole decode call tree on this thread.
    thread_local AssetDecodeInputFiles* t_DecodeInputFiles = nullptr;
} // namespace

ScopedAssetDecodeInputFiles::ScopedAssetDecodeInputFiles(AssetDecodeInputFiles* files)
    : m_Prev(t_DecodeInputFiles)
{
    t_DecodeInputFiles = files;
}

ScopedAssetDecodeInputFiles::~ScopedAssetDecodeInputFiles()
{
    t_DecodeInputFiles = m_Prev;
}

bool ReadAssetDecodeInputFromDisk(const AssetDecodeInput& input, ByteBuffer& bytes)
{
    if (!input.PrefixBytes)
        return ReadFileBytesShared(input.Path, bytes);

    bytes = ByteBuffer();
    SharedFileReader file(input.Path);
    if (!file.IsOpen())
        return false;
    const int64 size = file.Size();
    if (size < 0)
        return false;
    bytes = ByteBuffer(static_cast<size_t>(std::min(*input.PrefixBytes, static_cast<uint64>(size))));
    const int64 read = file.Read(bytes.Data(), bytes.Size());
    if (read < 0)
    {
        bytes = ByteBuffer();
        return false;
    }
    bytes.Truncate(static_cast<size_t>(read));
    return true;
}

const ByteBuffer* ReadAssetDecodeInputFile(const AssetDecodeInput& input, ByteBuffer& storage)
{
    if (t_DecodeInputFiles)
    {
        const auto file = t_DecodeInputFiles->Files.find(input.Path.generic_string());
        return file != t_DecodeInputFiles->Files.end() && file->second ? &*file->second : nullptr;
    }
    return ReadAssetDecodeInputFromDisk(input, storage) ? &storage : nullptr;
}

bool TakeAssetDecodeInputFile(const AssetDecodeInput& input, ByteBuffer& bytes)
{
    if (!t_DecodeInputFiles)
        return ReadAssetDecodeInputFromDisk(input, bytes);
    const auto file = t_DecodeInputFiles->Files.find(input.Path.generic_string());
    if (file == t_DecodeInputFiles->Files.end() || !file->second)
        return false;
    bytes = std::move(*file->second);
    file->second.reset();
    return true;
}

const std::any* CurrentAssetDecodePlan()
{
    return t_DecodeInputFiles ? &t_DecodeInputFiles->Plan : nullptr;
}

} // namespace GameEngine
