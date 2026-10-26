---
paths:
  - "Engine/Modules/Rendering/**"
  - "Engine/Source/Engine/Rendering/**"
  - "Engine/Include/Engine/Rendering/**"
  - "Engine/Modules/Ocean/**"
  - "Engine/Modules/CBTTerrain*/**"
  - "Engine/Modules/Sky/**"
  - "Apps/Editor/Source/SceneView/**"
  - "Apps/Editor/Source/DebugServer/RenderDocCapture.cpp"
  - "**/Shaders/**"
  - "**/*.glsl"
  - "**/*.comp"
  - "**/*.slang"
---

# Rendering

## Pipeline

- **Vertex format**: bitmask-driven (`VertexAttributeFlags`); `VertexLayoutBuilder` configures bindings from it.
- **MeshGPURegistry**: GUID-keyed dedup of GPU mesh buffers, generational handles.
- **Material system**: schema-driven properties; `ShaderComposer` generates modular adapter shaders with keyword variants; SPIR-V compilation through `MaterialBuildService`.
- **Draw batching**: GPU-driven. `GPUDrawStreamBuilder::ScheduleUnifiedScatter` scatters visible instances into indirect draw records per slice; batches key on `(colorClassId, poolGroup)`, so an extra instance in an existing batch costs no draw call. `WorldDrawBuilder` builds the per-frame submission.
- **GPU buffers**: `PerFrameWritePool`, double-buffered shared SSBOs for instance data, material parameters and bone palettes.
- **Render graph**: a DAG of passes with automatic resource tracking and barrier insertion. Transient resources are recycled: `RGTransientPool` returns an existing texture or buffer whenever the descriptor matches exactly, so a "fresh" resource can hold a previous user's contents; clear or fully overwrite before reading. Host-visible upload memory lives in `RGUploadRing` and never participates.
- **Forward+ lighting**: clustered light culling; lighting accumulated in view space (geometric camera centering, distinct from the precision technique below).
- **Camera-relative rendering** for Earth-scale precision: the render origin is an integer sector plus fp32 local, derived from the camera each frame (`RenderOrigin.h`). Each `GPUInstance` packs its sector into the two reserved words; the vertex stage differences it against the camera's sector and projects a render-origin-relative position through a rebased view-projection, so far-from-origin meshes stop jittering while the persistent instance buffer never re-uploads for camera motion. Inactive (sector 0) inside the activation radius is byte-identical to the full-world path.
- **Cascaded shadow maps**: four directional cascades. `ShadowMapRenderFeature` owns every shadow family and computes per-cascade light view-projections; the depth pass renders to a 2D array texture; `shadow_sampling.glsl` selects the cascade by view-space depth and samples through the hardware PCF comparison sampler.
- **Shaders**: GLSL in `Engine/Modules/Rendering/Shaders/`, composed through adapters in `Shaders/Adapters/`, compiled to SPIR-V with shaderc. One dynamic index into a local array makes glslang copy the whole array per invocation; large tables go in an SSBO.

## Vulkan conventions (they invert other engines' habits)

- Left-handed coordinates, +Z forward, +Y up: `MakeLookAtLH`, `MakePerspectiveLH_ZO_ReverseZ`, `MakeOrthographicLH_ZO_ReverseZ`.
- Reverse-Z: NDC depth is still `[0,1]`, but near maps to 1.0 and far to 0.0. The depth buffer is D32_SFLOAT (D24_UNORM is a precision regression under reverse-Z). `Rendering/Core/ReverseZ.h` names the convention: `kClearFar` (0.0) is the depth clear, `kNearerOrEqual` (`GreaterOrEqual`) the default depth test and the shadow comparison sampler, `kFarther` (`Less`) the x-ray passes. Production code uses these names, never a literal; tests follow on contact.
- Shadow depth bias is negative (`kShadowDepthBias{Constant,Slope} < 0`) to push fragments toward 0.0; receiver bias is added to NDC depth where forward-Z subtracts.
- Negative viewport height for Y-up rasterization (VK_KHR_maintenance1), independent of the Z direction; shadow sampling flips `shadowUV.y = 1.0 - shadowUV.y` to compensate.

## GPU debugging

Whenever the symptom is visual, prefer a capture over speculation. Start with data, not math: verify the GPU reads the buffer you think it does (write a known value; does the output change?). Most rendering bugs are data-pipeline bugs: wrong buffer bound, stale descriptor, aliased memory. Change one convention at a time (handedness, Y flip, depth range); simultaneous changes mask each other.

- **RenderDoc**: launch the editor with `launch_editor renderdoc=true` (runs `renderdoccmd capture`; default install `C:\Program Files\RenderDoc\`, `RENDERDOC_PATH` overrides), or by hand under `qrenderdoc` for the GUI's launch options; trigger captures with the `trigger_capture` MCP tool (`Apps/Editor/Source/DebugServer/RenderDocCapture.cpp`). `renderdoc=true` defaults `captureCompat=true` (`GE_VK_CAPTURE_COMPAT=1`), without which descriptor-buffer captures routinely fail to replay; that same setting makes HZB occlusion culling report zero culled draws, so pass `captureCompat=false` for occlusion or performance captures. Captures land at `<editor exe dir>/Captures/editor_frameN.rdc` (`GE_RENDERDOC_CAPTURE_DIR` overrides; a template set through `renderdoccmd --capture-file` is left alone); `get_capture_status` reports the newest capture. Analyse `.rdc` files with `qrenderdoc --python <script>` (`GameEngine-Workbench/measurements/geometry-deep-dive-2026-07-24/rd_census.py` is the pattern); qrenderdoc's first-run analytics dialog blocks `--python` until `%APPDATA%\qrenderdoc\UI.config` carries `"Analytics_TotalOptOut": true`.
- **RenderDoc opens a network listener, the engine does not.** A process launched under RenderDoc (`renderdoccmd`, `qrenderdoc`, `launch_editor renderdoc=true`) or with `renderdoc.dll` injected starts RenderDoc's target-control server bound to all interfaces, so the first such launch from a new executable path raises a Windows Firewall prompt and leaves an inbound Allow rule for that path. The engine's own listeners (the debug server, its wake socket) bind `127.0.0.1` only. A firewall prompt or rule on a RenderDoc-launched editor or test executable is RenderDoc's and is never "fixed" in the engine.
- **GPU-assisted validation is half usable.** With `VK_EXT_descriptor_buffer` enabled the validation layer disables all shader instrumentation (descriptor and BDA out-of-bounds checks, DebugPrintf) and logs it. The gate is the extension being enabled, not used: `GE_VK_USE_DESCRIPTOR_BUFFER=0` leaves it enabled and restores nothing; `GE_VK_CAPTURE_COMPAT=1` drops the extension and does. Shader instrumentation is requestable with `GE_VK_GPU_AV_SHADER_INSTRUMENTATION` (default `auto`); the resolved state is logged after device creation as ACTIVE, REQUESTED-BUT-UNAVAILABLE or NOT REQUESTED, so read that line. Buffer-content validation stays on and checks indirect draw, dispatch and trace-rays argument buffers, index buffers and copies, which cover this engine's GPU-driven draws and CBT's indirect dispatches: arm it with `GE_VK_GPU_AV=1` together with `GE_VK_VALIDATION=1` (DebugFast does not load the layer by default). It inserts a validation dispatch before each indirect command and does not restore descriptor-buffer bindings, so corroborate any finding against a `GE_VK_GPU_AV=0` run.
- **`VK_EXT_device_fault`** is on by default in Debug and DebugFast, opt-in in Release (`GE_VK_DEVICE_FAULT=1`; `=0` disarms). A device loss logs the fault address and vendor records with the resolved address range; addresses are precise to a power-of-two granule, so match a buffer's device address against the range, with the raw address as an extra hint.
- **`VK_EXT_debug_utils`** (labels and object names) is on in every configuration, independent of the validation layer, so captures and crash reports name render-graph passes in Release too; `GE_VK_NO_DEBUG_UTILS=1` drops it for non-debug instances. `get_gpu_tooling` reports `debugUtilsEnabled` and the attached tools.
- Guides: `docs/rendering/gpu-debugging-guide.mdx` (wrong pixels), `docs/contributing/device-loss-instrumentation.mdx` (`VK_ERROR_DEVICE_LOST` forensics and the Crash Diagnostic Layer procedure), `docs/rendering/external-gpu-profilers.mdx` (RenderDoc, Nsight Graphics, Nsight Systems, RGP and their trigger tools).
