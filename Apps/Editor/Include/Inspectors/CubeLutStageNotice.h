#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace GameEngine
{
class GUID;
struct InspectorContext;

namespace Components
{
enum class CubeLutStage : uint32_t;
}

namespace Rendering
{
enum class CubeLutInputEncoding : int32_t;
}

/// The text of the notice under a Cube LUT effect's Stage row when the LUT is made for the other
/// stage: a display LUT at Grade clips scene color above 1, and a log or scene LUT at Finish receives
/// display color and crushes the image. Empty when the stage suits the LUT (Rendering::CubeLutExpectsDisplayInput). The
/// notice's action, not its text, names the fix.
/// @param fileName the .cube file's name as the user spelled it, e.g. "Warm.cube"
/// @param inputMax the top of the table's input range
std::string CubeLutStageNoticeText(std::string_view fileName, Rendering::CubeLutInputEncoding encoding, float inputMax,
                                   Components::CubeLutStage stage);

/// Adds the notice for the LUT asset under ctx.Parent: an inspector notice whose one action ("Choose
/// Finish" or "Choose Grade") calls `chooseStage` with the stage the LUT is made for. When the asset is
/// not loaded, starts its load and, through ctx.FrameRefreshCallbacks, requests an inspector refresh
/// once the load completes, so the notice appears without another edit.
void AddCubeLutStageNotice(const InspectorContext& ctx, const GUID& lutGuid, Components::CubeLutStage stage,
                           std::function<void(Components::CubeLutStage)> chooseStage);

} // namespace GameEngine
