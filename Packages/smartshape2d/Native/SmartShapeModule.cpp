#include "SmartShape2D/ShapeRuntime.h"
#include "SmartShape2D/SmartShapeEdge.h"
#include "SmartShape2D/SmartShapeAnchor.h"
#include "SmartShape2D/ShapeSystem.h"
#include "SmartShape2D/SmartShapeCut.h"
#include "SmartShape2D/ShapeDestructionDemo.h"
#include "SmartShape2D/ShapeDestructionDemoSystem.h"
#include "SmartShape2D/ShapeExplosionFlash.h"
#include "Components/ComponentRegistrationLite.h"
#include "ECS/SystemScheduling.h"
#include "PluginAPI/EnginePlugin.h"

GE_REGISTER_COMPONENT_NO_ADD(GameEngine::Components::GeneratedShapeResources, Owner, MeshKey, Part)
GE_REFLECT_COMPONENT_DONOTSERIALIZE(GameEngine::Components::GeneratedShapeResources)

GE_REGISTER_COMPONENT_NO_ADD(GameEngine::Components::SmartShapeCut, Center, AxisX, AxisY)
GE_REGISTER_COMPONENT(GameEngine::Components::ShapeDestructionDemo, Radius, FlashMaterial)
GE_REFLECT_FIELD_RANGE(GameEngine::Components::ShapeDestructionDemo, Radius, 0.05f, 100.0f)
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::SmartShape, Destructible, "Allow explosion cuts to remove visible terrain and collision during gameplay. The authored spline stays intact.")
GE_REGISTER_COMPONENT_NO_ADD(GameEngine::Components::ShapeExplosionFlash, Remaining)
GE_REFLECT_COMPONENT_DONOTSERIALIZE(GameEngine::Components::ShapeExplosionFlash)

GE_REGISTER_COMPONENT(GameEngine::Components::SmartShapeEdge, PointIndex, UseNormalRange, NormalStart, NormalSweep,
    Priority, Material, WidthScale, Flip, Visible, TextureIndex, TextureSet, OverrideTextureSettings,
    RandomizeTextures, StretchToFit, UseCornerTextures, UseTaperTextures, Offset, DepthOffset, Weld, TaperSharpCorners)
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::SmartShapeEdge, UseNormalRange, "Apply to outward normals within an angular range instead of one point. Point overrides take precedence.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::SmartShapeEdge, NormalStart, "Start angle in degrees, counterclockwise from local +X. +Y is 90 degrees.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::SmartShapeEdge, NormalSweep, "Signed angular span. Zero or 360 selects all directions. Every matching normal rule renders its own layer.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::SmartShapeEdge, TextureSet, "Select texture entries with this set number. Point-specific entries replace the default entries within that set.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::SmartShapeEdge, OverrideTextureSettings, "Use this edge rule's randomization, fit, corner and taper settings instead of the shape settings.")
GE_REFLECT_FIELD_RANGE(GameEngine::Components::SmartShapeEdge, WidthScale, 0.001f, 1000.0f)
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::SmartShapeEdge, PointIndex, "Zero-based spline point whose outgoing segment is overridden. Parent this entity to a Smart Shape.")

GE_REGISTER_COMPONENT(GameEngine::Components::SmartShapeAnchor, PointIndex, PointOffset, Rotation, UseShapeScale)
GE_REFLECT_FIELD_RANGE(GameEngine::Components::SmartShapeAnchor, PointOffset, 0.0f, 1.0f)
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::SmartShapeAnchor, Rotation, "Rotation in degrees relative to the forward curve tangent. Parent this entity to a Smart Shape.")

GE_REGISTER_COMPONENT(GameEngine::Components::SmartShapeTexture, PointIndex, Index, TextureSet, Material, InnerCornerMaterial,
    OuterCornerMaterial, StartTaperMaterial, EndTaperMaterial, SharpStartTaperMaterial, SharpEndTaperMaterial,
    TileLength, WidthScale, StartTaperSize, EndTaperSize)
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::SmartShapeTexture, PointIndex, "Segment receiving this texture set entry. -1 supplies the shape default set. Parent this entry to a Smart Shape.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::SmartShapeTexture, Index, "Order within the set. Add as many entries as needed; selection is stable for a fixed texture seed.")

GE_REGISTER_COMPONENT_BEGIN(GameEngine::Components::SmartShape)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, FillMaterial)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, TopMaterial)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, RightMaterial)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, BottomMaterial)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, LeftMaterial)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, InnerCornerMaterial)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, OuterCornerMaterial)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, StartCapMaterial)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, EndCapMaterial)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, FillScale)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, FillOffset)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, FillRotation)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, FillAbsolutePosition)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, FillAbsoluteRotation)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, FillDepth)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, FillMeshOffset)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, EdgeWidth)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, EdgeOffset)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, TileLength)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, EdgeDepth)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, CurveSubdivisions)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, CollisionDepth)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, CollisionWidth)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, CollisionOffset)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, CollisionMode)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, CollisionEnabled)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, Destructible)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, FillEnabled)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, EdgesEnabled)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, StretchToFit)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, FlipEdges)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, RandomizeTextures)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, TextureSeed)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, WeldEdges)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, UseCornerTextures)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, UseTaperTextures)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, TaperSharpCorners)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, StartTaperSize)
    GE_REFLECT_FIELD(GameEngine::Components::SmartShape, EndTaperSize)
GE_REGISTER_COMPONENT_END(GameEngine::Components::SmartShape)

GE_REFLECT_ENUM_TABLE_BEGIN(SmartShapeCollisionModes)
    GE_REFLECT_ENUM_TABLE_VALUE("Filled", 0)
    GE_REFLECT_ENUM_TABLE_VALUE("Rendered edges", 1)
    GE_REFLECT_ENUM_TABLE_VALUE("Hollow", 2)
GE_REFLECT_ENUM_TABLE_END()
GE_REFLECT_ENUM_FIELD(GameEngine::Components::SmartShape, CollisionMode, SmartShapeCollisionModes)
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::SmartShape, CollisionMode, "Fill the outline, follow the rendered edges, or leave a hollow interior. Open paths use a butt-ended ribbon.")
GE_REFLECT_FIELD_RANGE(GameEngine::Components::SmartShape, CollisionDepth, 0.001f, 10000.0f)
GE_REFLECT_FIELD_RANGE(GameEngine::Components::SmartShape, EdgeWidth, 0.001f, 10000.0f)
GE_REFLECT_FIELD_RANGE(GameEngine::Components::SmartShape, TileLength, 0.001f, 10000.0f)
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::SmartShape, CurveSubdivisions, "Samples per curved segment, from 1 to 64. Straight segments use one sample.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::SmartShape, EdgeWidth, "Edge thickness in world units, multiplied by each spline point's radius.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::SmartShape, FillScale, "World units per texture repetition on local X and Y. Both values must be positive.")
GE_REFLECT_FIELD_TOOLTIP(GameEngine::Components::SmartShape, StretchToFit, "Fit a whole number of texture repeats to the outline. Off crops the last repeat.")

namespace GameEngine::SmartShape2D
{
namespace
{
class SmartShapePlugin final : public Plugins::IEnginePlugin
{
public:
    SmartShapePlugin() { Plugins::EnginePluginRegistry::Get().RegisterPlugin(*this); }
    const Plugins::PluginDescriptor& GetDescriptor() const override
    {
        static const Plugins::PluginDescriptor kDescriptor{"smartshape2d", "Smart Shape 2D", "1.0.0", true};
        return kDescriptor;
    }
    void AddSystemsToSchedule(ECS::SystemScheduleBuilder& builder, Plugins::EnginePluginContext&) override
    {
        // Exclusive Early work creates children before the hierarchy composes their transforms.
        builder.Add<ShapeDestructionDemoSystem>("SmartShapeDestructionDemo", ECS::SystemPhase::Early, -110, {});
        builder.Add<ShapeSystem>("SmartShape2D", ECS::SystemPhase::Early, -100, {});
    }
    void OnRuntimeInitialized(Plugins::EnginePluginContext& context) override
    {
        ShapeRuntime::Get().Initialize(context.RenderServices);
    }
    void OnRuntimeShutdown() override { ShapeRuntime::Get().Shutdown(); }
};
SmartShapePlugin s_Plugin;
} // namespace
} // namespace GameEngine::SmartShape2D
