#pragma once

#include "Assets/ParserRegistry.h"
#include "Assets/SceneAsset.h"

#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace GameEngine
{

class SceneAssetParser final : public AssetParser
{
  public:
    // One parser per scene-text kind: `type` is AssetType::Scene for ".scene" and
    // AssetType::Prefab for ".prefab".
    SceneAssetParser(AssetType type, std::string extension) : m_Type(type), m_Extension(std::move(extension)) {}

    AssetType GetAssetType() const override { return m_Type; }

    std::vector<std::string> GetSupportedExtensions() const override { return {m_Extension}; }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        try
        {
            auto asset = std::make_shared<SceneAsset>(metadata.Guid, metadata.Path, m_Type);
            return AssetParseResult(asset);
        }
        catch (const std::exception& e)
        {
            return AssetParseResult(false, "Failed to create Scene asset: " + std::string(e.what()));
        }
    }

    std::string GetName() const override { return "SceneAssetParser"; }

    int GetPriority() const override { return 30; }

    // Walk the .scene INI text and emit SceneEntityComponent dep edges. Each
    // `Component.field = "<guid>"` line under an `[entity id="..."]` section
    // becomes one edge with FieldLocator "entities[<id>].Component.field"; under
    // a `[prefab id="..." source="..."]` instance section, "prefabs[<id>]...".
    // Path-form values (rare in scenes today, but allowed) become path-form
    // edges and resolve at query time. Each `[resource id="..." ...]` header,
    // which names a prefab the scene's instances spawn or an asset a `#id`
    // field value uses, becomes a SceneResource edge with FieldLocator
    // "resources[<id>]". A header or `[path=... guid=...]` value that carries
    // both a GUID and a path becomes two edges, as the load falls back to the
    // path when the GUID is unknown. In a .prefab every edge is a PrefabComponent
    // edge instead: its components' asset references, and its [resource] headers,
    // which name its nested prefabs and a variant's base.
    bool ExtractDependencies(const GUID& referrer,
                             const AssetMetadata& metadata,
                             DepEdgeSink& sink) const override;

    // Add to `outNames` every component a .scene or .prefab body assigns a field
    // of: the `Component` of each `Component.field = value` line under an
    // [entity ...] or [prefab ...] instance section, read as ExtractDependencies
    // reads them.
    static void CollectComponentNames(std::string_view body, std::unordered_set<std::string>& outNames);

    // A .scene or .prefab body without the lines of the components `isEditorOnly`
    // names: every `Component.field = value` line of such a component under an [entity ...]
    // or [prefab ...] instance section, and every `-Component`, `+Component` or bare
    // `Component` line, each with or without an override's `address/` in front. On an entity that
    // carries a mark-up (a `Markup` line), its `Name` and `Spline` lines go too: the
    // title is conversation and a path mark-up's spline is its shape. Section headers
    // stay, so every entity, its parent link and its other components survive; every
    // other line is kept byte for byte.
    static std::string StripEditorOnlyComponents(std::string_view body,
                                                 const std::function<bool(std::string_view)>& isEditorOnly);

  private:
    AssetType m_Type;
    std::string m_Extension;
};

} // namespace GameEngine
