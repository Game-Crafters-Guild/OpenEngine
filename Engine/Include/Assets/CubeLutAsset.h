#pragma once

#include "AssetCore/Asset.h"
#include "Rendering/Utils/CubeLutColorDomain.h"
#include "Types/Types.h"

#include <cstdint>
#include <string>

namespace GameEngine
{

/// The header of a .cube file: what it declares about its tables.
struct CubeLutFileHeader
{
    std::string Title;     ///< the TITLE line, empty when the file has none
    uint32_t Size3D = 0;   ///< LUT_3D_SIZE, 0 without a 3D table
    uint32_t Size1D = 0;   ///< LUT_1D_SIZE (a shaper when both are present), 0 without one
    float32 InputMin = 0.0f; ///< the input range the tables cover (DOMAIN_* or LUT_*_INPUT_RANGE)
    float32 InputMax = 1.0f;
};

/// A Resolve .cube color LUT (.cube). Loading reads the file's header and tables and keeps the
/// header and the importer's proposal of its input encoding (Rendering::ProposeCubeLutColor). The
/// tables themselves are uploaded by TextureService, which reads the same file (issue #4186); a
/// user's choice of encoding is stored on the asset (ResolveCubeLutColorSettings).
class CubeLutAsset final : public Asset
{
  public:
    CubeLutAsset(const GUID& guid, const std::filesystem::path& path)
        : Asset(guid, AssetType::CubeLut, path)
    {
    }

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    const CubeLutFileHeader& GetHeader() const { return m_Header; }
    const Rendering::CubeLutColorProposal& GetProposal() const { return m_Proposal; }

  private:
    bool ParseText(std::string_view text);

    CubeLutFileHeader m_Header{};
    Rendering::CubeLutColorProposal m_Proposal{};
};

} // namespace GameEngine
