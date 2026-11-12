#include "PavingApproachGround.h"

#include "Components/GeneratedEntity.h"
#include "Components/Hierarchy.h"
#include "Components/Spline/SplineExtrude.h"
#include "Components/Spline/SplineNetworkMember.h"
#include "Components/Spline/SplinePaving.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Query.h"
#include "ECS/World.h"
#include "Mathematics/Matrix4x4.h"
#include "SplineECS/SplineService.h"
#include "SplineLayout/PieceEntity.h"
#include "Types/FlatMap.h"

#include <cstring>

namespace GameEngine::SplineRecipes
{
namespace
{

using Components::SplineGroundAuthority;

// Two copies of one trivially copyable effect read the same, byte for byte.
template <typename Effect>
bool SameEffect(const std::optional<Effect>& a, const std::optional<Effect>& b)
{
    if (a.has_value() != b.has_value())
        return false;
    return !a || std::memcmp(&*a, &*b, sizeof(Effect)) == 0;
}

template <typename Effect>
std::optional<Effect> CopyOf(const ECS::World& world, ECS::EntityHandle entity)
{
    const Effect* effect = world.GetComponent<Effect>(entity);
    return effect ? std::optional<Effect>(*effect) : std::nullopt;
}

// True when a column of `Effect` changed since `gate`.
template <typename Effect>
bool ColumnChanged(ECS::World& world, const ECS::ChangeGate& gate)
{
    bool changed = false;
    auto query = world.Query<ECS::Read<Effect>>();
    query.template Changed<Effect>(gate);
    query.Each([&changed](const Effect&) { changed = true; });
    return changed;
}

// A paved area whose ground SplineGroundAuthorityController provisions, resolved to Owns.
bool OwnsItsGround(const ECS::World& world, ECS::EntityHandle entity)
{
    const auto* paving = world.GetComponent<Components::SplinePaving>(entity);
    const auto* member = world.GetComponent<Components::SplineNetworkMember>(entity);
    return paving && member && member->Kind == Components::SplineNetworkMemberKind::Area
           && world.GetComponent<Components::SplineExtrude>(entity) == nullptr
           && Components::ResolveGroundAuthority(*paving) == SplineGroundAuthority::Owns;
}

// Each road's own ground authority, by entity id. A road with no extrude is not in the map.
FlatMap<uint32, SplineGroundAuthority> RoadAuthorities(ECS::World& world)
{
    FlatMap<uint32, SplineGroundAuthority> roads;
    world.Query<ECS::Read<Components::SplineExtrude>>().Each(
        [&roads](ECS::EntityHandle entity, const Components::SplineExtrude& recipe)
        { roads.InsertOrAssign(entity.id, Components::ResolveGroundAuthority(recipe)); });
    return roads;
}

} // namespace

bool PavingApproachGround::InputsChanged(ECS::World& world, uint64 approachesVersion) const
{
    if (!m_Primed || !ECS::ChangeFilter::Enabled() || approachesVersion != m_ApproachesVersion
        || world.GetStructuralChangeVersion() != m_StructuralVersion)
        return true;
    return ColumnChanged<Components::SplinePaving>(world, m_Gate)
           || ColumnChanged<Components::SplineExtrude>(world, m_Gate)
           || ColumnChanged<Components::TerrainModifierVolume>(world, m_Gate)
           || ColumnChanged<Components::TerrainGrassEffect>(world, m_Gate)
           || ColumnChanged<Components::TerrainPaintLayerEffect>(world, m_Gate)
           || ColumnChanged<Components::TerrainFlattenEffect>(world, m_Gate)
           || ColumnChanged<Components::TerrainGroundClaimEffect>(world, m_Gate);
}

std::vector<PavingApproachGround::Piece> PavingApproachGround::Wanted(ECS::World& world,
                                                                      std::span<const PavingApproach> approaches) const
{
    std::vector<Piece> wanted;
    if (approaches.empty())
        return wanted;
    const FlatMap<uint32, SplineGroundAuthority> roads = RoadAuthorities(world);
    for (const PavingApproach& approach : approaches)
    {
        if (!world.IsValid(approach.Owner) || !OwnsItsGround(world, approach.Owner))
            continue;
        const auto* volume = world.GetComponent<Components::TerrainModifierVolume>(approach.Owner);
        const SplineGroundAuthority* road = roads.Find(approach.Road);
        const SplineGroundAuthority roadAuthority = road ? *road : SplineGroundAuthority::None;
        if (!volume || roadAuthority == SplineGroundAuthority::Owns)
            continue;
        Piece piece{.Approach = approach};
        Copied& copied = piece.Effects;
        copied.Grass = CopyOf<Components::TerrainGrassEffect>(world, approach.Owner);
        copied.Paint = CopyOf<Components::TerrainPaintLayerEffect>(world, approach.Owner);
        // The paving grades the arm of a road that does not grade its own ground.
        if (roadAuthority != SplineGroundAuthority::Grades)
        {
            copied.Flatten = CopyOf<Components::TerrainFlattenEffect>(world, approach.Owner);
            copied.Claim = CopyOf<Components::TerrainGroundClaimEffect>(world, approach.Owner);
        }
        copied.Falloff = volume->Falloff;
        copied.Weight = volume->Weight;
        copied.Priority = volume->Priority;
        if (copied.Grass || copied.Paint || copied.Flatten || copied.Claim)
            wanted.push_back(std::move(piece));
    }
    return wanted;
}

bool PavingApproachGround::Laid(const ECS::World& world, std::span<const Piece> wanted) const
{
    if (wanted.size() != m_Pieces.size())
        return false;
    for (std::size_t i = 0; i < wanted.size(); ++i)
    {
        const Copied& next = wanted[i].Effects;
        const Copied& laid = m_Pieces[i].Effects;
        if (!world.IsValid(m_Pieces[i].Entity) || !(wanted[i].Approach == m_Pieces[i].Approach)
            || !SameEffect(next.Grass, laid.Grass) || !SameEffect(next.Paint, laid.Paint)
            || !SameEffect(next.Flatten, laid.Flatten) || !SameEffect(next.Claim, laid.Claim)
            || next.Falloff != laid.Falloff || next.Weight != laid.Weight || next.Priority != laid.Priority)
            return false;
    }
    return true;
}

void PavingApproachGround::Lay(ECS::World& world, Piece& piece, uint32 index) const
{
    auto* splines = SplineECS::SplineService::TryGet();
    const auto* ownerWorld = world.GetComponent<Components::WorldTransform>(piece.Approach.Owner);
    if (!splines || !ownerWorld)
        return;
    const Components::WorldTransform pose = *ownerWorld;

    // The spline lies in the paving's space: the piece is its child at the identity pose.
    const Mathematics::Matrix4x4 toLocal = SplineLayout::InvertPlacerWorld(
        pose.ToMatrix4x4(pose.GetPosition() - Mathematics::WorldPosition{}).Data());
    const SplineECS::SplineHandle handle = splines->CreateSpline(Spline::SplineType::Linear, false);
    auto* data = splines->GetSplineData(handle);
    for (std::size_t i = 0; i < piece.Approach.Points.size(); ++i)
        data->AddPoint(toLocal.TransformPoint(piece.Approach.Points[i]), piece.Approach.HalfWidths[i]);
    splines->RebuildCache(handle);

    Components::SplineComponent spline{};
    spline.SplineDataIndex = handle.Index();
    spline.SplineDataGeneration = handle.Generation();
    Components::TerrainModifierVolume volume{};
    volume.Shape = Components::TerrainVolumeShape::SplinePath;
    volume.Falloff = piece.Effects.Falloff;
    volume.Weight = piece.Effects.Weight;
    volume.Priority = piece.Effects.Priority;

    ECS::Entity entity = world.Create();
    entity.Set(Components::Transform{});
    entity.Set(pose);
    entity.Set(Components::GeneratedEntity{});
    entity.Set(Components::Parent{piece.Approach.Owner});
    entity.Set(SplineLayout::MakePieceLabel("Approach", index));
    entity.Set(spline);
    entity.Set(volume);
    if (piece.Effects.Grass)
        entity.Set(*piece.Effects.Grass);
    if (piece.Effects.Paint)
        entity.Set(*piece.Effects.Paint);
    if (piece.Effects.Flatten)
        entity.Set(*piece.Effects.Flatten);
    if (piece.Effects.Claim)
        entity.Set(*piece.Effects.Claim);
    piece.Entity = entity.GetHandle();
    piece.Spline = spline;
}

void PavingApproachGround::Update(ECS::World& world, std::span<const PavingApproach> approaches,
                                  uint64 approachesVersion)
{
    if (approaches.empty() && m_Pieces.empty())
        return;
    // Sampled before the reads: a write during this call is seen on the next one.
    const uint64 entryVersion = world.GetGlobalSystemVersion();
    if (!InputsChanged(world, approachesVersion))
        return;
    ++m_Reads;

    std::vector<Piece> wanted = Wanted(world, approaches);
    if (!Laid(world, wanted))
    {
        Shutdown(world);
        for (uint32 i = 0; i < wanted.size(); ++i)
            Lay(world, wanted[i], i);
        m_Pieces = std::move(wanted);
    }
    m_Gate.LastRunVersion = entryVersion;
    m_StructuralVersion = world.GetStructuralChangeVersion();
    m_ApproachesVersion = approachesVersion;
    m_Primed = true;
}

void PavingApproachGround::Shutdown(ECS::World& world)
{
    auto* splines = SplineECS::SplineService::TryGet();
    for (const Piece& piece : m_Pieces)
    {
        if (splines)
            splines->DestroySpline(SplineECS::SplineHandle(piece.Spline.SplineDataIndex, piece.Spline.SplineDataGeneration));
        if (world.IsValid(piece.Entity))
            world.DestroyEntity(piece.Entity);
    }
    m_Pieces.clear();
}

} // namespace GameEngine::SplineRecipes
