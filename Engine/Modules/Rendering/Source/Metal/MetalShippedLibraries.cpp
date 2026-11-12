#include "MetalShippedLibraries.h"

#include "MetalShaderTranslator.h"

#include "Logger/Logger.h"

#include <Metal/Metal.hpp>

#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <span>
#include <string>
#include <vector>

namespace GameEngine::Rendering
{

MetalShippedLibraries::Loaded::~Loaded()
{
    if (Mapping != nullptr)
        munmap(Mapping, Size);
}

std::shared_ptr<const MetalShippedLibraries::Loaded> MetalShippedLibraries::Acquire(
    const std::filesystem::path& archive, uint64_t optionsHash)
{
    std::lock_guard lock(m_Mutex);
    if (archive == m_Archive)
        return m_Loaded;
    m_Archive = archive;
    m_Loaded.reset();
    const int file = open(archive.c_str(), O_RDONLY | O_CLOEXEC);
    if (file < 0)
        return nullptr;
    auto loaded = std::make_shared<Loaded>();
    struct stat info{};
    if (fstat(file, &info) == 0 && info.st_size > 0)
    {
        void* mapping = mmap(nullptr, static_cast<size_t>(info.st_size), PROT_READ, MAP_PRIVATE, file, 0);
        if (mapping != MAP_FAILED)
        {
            loaded->Mapping = mapping;
            loaded->Size = static_cast<size_t>(info.st_size);
        }
    }
    close(file);
    if (loaded->Mapping == nullptr && info.st_size > 0)
    {
        Logger::Log::Warning("MetalDevice: shipped libraries '{}' not used: the file cannot be mapped", archive.string());
        return nullptr;
    }
    const std::span<const uint8_t> bytes(static_cast<const uint8_t*>(loaded->Mapping), loaded->Size);
    uint64_t archiveOptionsHash = 0;
    std::vector<MetalLibraryArchiveEntry> entries;
    std::string error;
    if (!ReadMetalLibraryArchive(bytes, archiveOptionsHash, entries, error))
    {
        Logger::Log::Warning("MetalDevice: shipped libraries '{}' not used: {}", archive.string(), error);
        return nullptr;
    }
    if (archiveOptionsHash != optionsHash)
    {
        Logger::Log::Warning("MetalDevice: shipped libraries '{}' not used: built under other translator or "
                             "compile options; every stage compiles at run time",
                             archive.string());
        return nullptr;
    }
    for (const MetalLibraryArchiveEntry& entry : entries)
        loaded->Entries.InsertOrAssign(entry.Key, entry);
    Logger::Log::Info("MetalDevice: {} shipped stage libraries in '{}'", entries.size(), archive.string());
    m_Loaded = std::move(loaded);
    return m_Loaded;
}

MTL::Function* MetalShippedLibraries::NewFunction(MTL::Device* device, const std::filesystem::path& archive,
                                                  uint64_t optionsHash, uint64_t key,
                                                  MetalShaderTranslation& outTranslation)
{
    if (archive.empty())
        return nullptr;
    const std::shared_ptr<const Loaded> loaded = Acquire(archive, optionsHash);
    const MetalLibraryArchiveEntry* entry = loaded != nullptr ? loaded->Entries.Find(key) : nullptr;
    if (entry == nullptr)
        return nullptr;
    // DISPATCH_DATA_DESTRUCTOR_DEFAULT copies the bytes, so the library never points into the mapping.
    dispatch_data_t data = dispatch_data_create(entry->Library.data(), entry->Library.size(), nullptr,
                                                DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NS::Error* error = nullptr;
    MTL::Library* library = device->newLibrary(data, &error);
    dispatch_release(data);
    if (library == nullptr)
    {
        Logger::Log::Warning("MetalDevice: shipped library {:016x} did not load: {}", key,
                             error != nullptr ? error->localizedDescription()->utf8String() : "unknown error");
        return nullptr;
    }
    MTL::FunctionConstantValues* constants = MTL::FunctionConstantValues::alloc()->init();
    MTL::Function* function =
        library->newFunction(NS::String::string("main0", NS::UTF8StringEncoding), constants, &error);
    constants->release();
    library->release();
    if (function == nullptr)
        return nullptr;
    outTranslation.EntryPoint = "main0";
    outTranslation.LocalSizeX = entry->LocalSizeX;
    outTranslation.LocalSizeY = entry->LocalSizeY;
    outTranslation.LocalSizeZ = entry->LocalSizeZ;
    outTranslation.Success = true;
    return function;
}

} // namespace GameEngine::Rendering
