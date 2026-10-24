// Which pass an instance belongs to, read from its GPUInstance flags and the
// dispatch's GPUCullingData.passFlags. Shared by frustum_culling.comp and
// hzb_culling.comp.

// Mirror Rendering::kInstanceFlagShadowsOnly and kInstanceFlagEditorHidden
// (GPUInstanceDepthClass.h) and kCullPassShadowDispatch and
// kCullPassDropEditorHidden (GPUScene.h).
const uint kInstanceFlagShadowsOnly = 1u << 7;
const uint kInstanceFlagEditorHidden = 1u << 8;
const uint kCullPassShadowDispatch = 1u << 0;
const uint kCullPassDropEditorHidden = 1u << 1;

// A dispatch of the editor Scene View (kCullPassDropEditorHidden) drops what
// its eye hides, bit 8, from its color and its shadow passes alike. Otherwise
// a shadow dispatch keeps casters, bit 0, and a color dispatch keeps every
// instance that is not shadows-only, bit 7.
bool InstanceInPass(uint instanceFlags, uint passFlags)
{
    bool shadow = (passFlags & kCullPassShadowDispatch) != 0u;
    uint required = shadow ? 1u : 0u;
    uint dropped = shadow ? 0u : kInstanceFlagShadowsOnly;
    if ((passFlags & kCullPassDropEditorHidden) != 0u)
        dropped |= kInstanceFlagEditorHidden;
    return (instanceFlags & (required | dropped)) == required;
}
