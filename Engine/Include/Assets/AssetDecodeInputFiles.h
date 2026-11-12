#pragma once

#include "Types/ByteBuffer.h"
#include "Types/Types.h"

#include <any>
#include <filesystem>
#include <optional>

namespace GameEngine {

/// A file one decode reads besides its asset's own bytes. PrefixBytes limits the read to the
/// file's first bytes (a glTF buffer uses only its declared byteLength); nullopt reads it whole.
struct AssetDecodeInput
{
    std::filesystem::path Path;
    std::optional<uint64> PrefixBytes;
};

/// What a parser names on the reader thread for one decode (AssetParser::ResolveDecodeInputs, or
/// AssetParser::ResolveDecodeInputsWithoutBytes before the read).
struct AssetDecodeInputs
{
    /// The files the decode reads besides its asset's own bytes.
    Vector<AssetDecodeInput> Files;
    /// What the parser resolved to name them (a texture's cook plan), or empty. The decode takes it
    /// (CurrentAssetDecodePlan) instead of resolving it again, so it uses the files the reader read
    /// even when the state they were resolved from has changed since.
    std::any Plan;
};

/// What the AssetIOService's reader thread prepared for one decode before it: each file the parser
/// named, keyed by its path's generic string, with its bytes or nullopt when the file was absent or
/// could not be read; and the parser's plan (AssetDecodeInputs::Plan).
struct AssetDecodeInputFiles
{
    FastHashMap<String, std::optional<ByteBuffer>> Files;
    std::any Plan;
};

/// Publishes to this thread's decode call tree the files the AssetIOService read for it, and the parser's plan;
/// inside, ReadAssetDecodeInputFile serves only those files. A synchronous Load publishes null, so a Load nested
/// in a decode (an authored LOD slot's model) reads the disk and never takes the enclosing decode's files or plan.
class ScopedAssetDecodeInputFiles
{
  public:
    /// @param files The files the reader read for this decode; null = this call tree reads the disk.
    explicit ScopedAssetDecodeInputFiles(AssetDecodeInputFiles* files);
    ~ScopedAssetDecodeInputFiles();

    ScopedAssetDecodeInputFiles(const ScopedAssetDecodeInputFiles&) = delete;
    ScopedAssetDecodeInputFiles& operator=(const ScopedAssetDecodeInputFiles&) = delete;

  private:
    AssetDecodeInputFiles* m_Prev;
};

/// Reads `input` from the disk through the shared-open path (AssetCore/SharedFileRead.h): the whole file, or at
/// most its first PrefixBytes. The AssetIOService's reader and ReadAssetDecodeInputFile outside a decode use it.
/// @return False when the file cannot be opened or read.
bool ReadAssetDecodeInputFromDisk(const AssetDecodeInput& input, ByteBuffer& bytes);

/// The bytes of `input` for this thread's decode, valid for the publishing scope or for `storage`. Inside an
/// AssetIOService decode: what its reader read, or null for a path it did not read, found absent, or that was
/// taken; it never reads the disk. Anywhere else: the file, read into `storage`, or null when it cannot be read.
const ByteBuffer* ReadAssetDecodeInputFile(const AssetDecodeInput& input, ByteBuffer& storage);

/// Moves the bytes of `input` into `bytes`, for a file the decode keeps (a glTF's external image). Inside an
/// AssetIOService decode: what its reader read, and the path then reads as absent for the rest of that decode.
/// Anywhere else: the file, read from the disk. @return False when the file is absent or cannot be read.
bool TakeAssetDecodeInputFile(const AssetDecodeInput& input, ByteBuffer& bytes);

/**
 * @brief The plan the parser resolved on the reader thread for the decode running on this thread
 * (AssetDecodeInputs::Plan), empty when it resolved none; null outside a decode the
 * AssetIOService runs and in a synchronous Load nested in one.
 */
const std::any* CurrentAssetDecodePlan();

} // namespace GameEngine
