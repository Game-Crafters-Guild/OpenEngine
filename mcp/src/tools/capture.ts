import { z } from "zod";
import { editorCall } from "../editor-call.js";
import { defineTool, type ToolDef, type ToolOutput } from "../registry.js";
import { formatCaptureRecord, recordFromBytes, recordFromFile } from "./capture-record.js";
import { proxyTool } from "./proxy.js";

/**
 * Split a capture response into the PNG and everything else. The base64 payload
 * never travels in `data`: the CLI writes it to disk and MCP sends it as an
 * image block only when `inline`, so leaving it in the structured result would
 * only mean megabytes of base64 on stdout.
 */
function splitImage(result: any, inline: boolean): { image: NonNullable<ToolOutput["image"]>; data: Record<string, unknown> } {
  const { pngBase64, ...rest } = result;
  return { image: { base64: pngBase64, mimeType: "image/png", filePath: result.filePath, inline }, data: rest };
}

export const tools: ToolDef[] = [
  defineTool({
    name: "take_screenshot",
    category: "capture",
    ipcMethod: "take_screenshot",
    description: "Capture a PNG screenshot of the editor. Default target is the entire editor window (UI + viewport composited). Other targets crop to a specific region of the same composite. Returns the record line: the file path where the PNG was saved, its size in bytes, its SHA-256, the scene view camera pose and the graphics validation counts. The image is not in the response: open the file to look at it, or pass includeImage=true when the client cannot read files. " +
      "BEFORE DRAWING A VISUAL CONCLUSION, CHECK `pendingMaterialTextureBinds`. It is 0 on a settled frame, and non-zero while that many material texture slots are still waiting on a decode — those materials render their bindless default (white albedo, flat normal) instead of the authored texture. " +
      "On screen the two states are identical, so a capture taken inside that window reads as 'this material never binds'. A texture's first material bind also tags its cook usage, so on a cold derived cache the window is as wide as the block-compression encode: seconds to minutes, not frames. " +
      "Poll get_editor_state until it reports 0 before capturing anything you intend to measure or A/B. " +
      "The capture is the render graph's own frame (`method: \"rendergraph\"`). When the editor cannot produce one — the scene view's tab is behind another, the window is minimized, the frame loop is starved — the call fails with the reason rather than substituting a picture of the window; pass allowWindowCapture=true if a window picture is what you want.",
    schema: {
      target: z.enum(["window", "viewport", "panel", "element", "rect", "asset_preview"]).optional()
        .describe("What to capture: 'window' (full editor, default), 'viewport' (scene view only), 'panel' (a docked panel), 'element' (a UI element by id), 'rect' (custom rectangle in window pixels), or 'asset_preview' (live GPU video frame from Asset View — works on macOS when UI composite readback is unavailable)."),
      panelId: z.string().optional().describe("Required when target='panel'. Dock panel ID: 'Assets', 'AssetView', 'Hierarchy', 'Inspector', etc. Use get_panel_tree to discover bounds."),
      elementId: z.string().optional().describe("Required when target='element'. UI element id (matches FindById). Asset View preview image: 'asset-view-preview-image'."),
      x: z.coerce.number().int().optional().describe("target='rect': left edge."),
      y: z.coerce.number().int().optional().describe("target='rect': top edge."),
      w: z.coerce.number().int().optional().describe("target='rect': width."),
      h: z.coerce.number().int().optional().describe("target='rect': height."),
      coords: z.enum(["logical", "physical"]).optional()
        .describe("target='rect' coordinate space. 'logical' (default) matches get_ui_tree / get_panel_tree output and panel/element bounds. 'physical' is the saved PNG's pixel grid (useful for cropping a previously-saved screenshot)."),
      includeImage: z.boolean().optional()
        .describe("Also return the PNG inline as an image block (default false: the record line only). For a client that cannot open the saved file."),
      allowWindowCapture: z.boolean().optional()
        .describe("Accept an OS window capture (method: 'printwindow', with fallbackReason) when the render graph cannot hand over the frame. Default false: an unavailable frame is an error naming the reason, because a window picture looks like a valid capture while showing whatever the compositor holds — including other windows' pixels and stale content."),
    },
    run: async ({ includeImage, ...args }) => {
      const params: Record<string, unknown> = {};
      for (const [k, v] of Object.entries(args)) if (v !== undefined) params[k] = v;
      const result = await editorCall("take_screenshot", params) as any;
      if (!result?.pngBase64) return { data: result };
      const tgt = result.target ?? args.target ?? "window";
      const { image, data } = splitImage(result, includeImage ?? false);
      const record = recordFromBytes(result, Buffer.from(image.base64, "base64"));
      // Surfaced in the summary, not just the payload: an unsettled frame is a
      // plausible PNG, never an error, so the warning has to reach the reader
      // who did not think to look for it.
      const pending = Number(result.pendingMaterialTextureBinds ?? 0);
      const warn = pending > 0
        ? ` — WARNING: ${pending} material texture bind(s) still pending; those materials are showing bindless defaults, not their textures`
        : "";
      // Same reason the pending-bind warning is here: a window capture is a
      // plausible PNG of the wrong thing, so the method has to reach the reader
      // rather than sit in the payload.
      const substitute = result.method === "printwindow"
        ? ` — WARNING: this is an OS window capture, not the rendered frame (${result.fallbackReason ?? "reason not stated"})`
        : "";
      return {
        data: { ...data, ...record },
        image,
        summary: `${formatCaptureRecord(`Screenshot (${tgt}) ${result.width}x${result.height}`, record)}${warn}${substitute}`,
      };
    },
  }),

  defineTool({
    name: "capture_resource",
    category: "capture",
    ipcMethod: "capture_resource",
    description: "Capture a named render graph resource from the live frame: a texture as a PNG, a buffer as bytes. Use get_render_graph_resources to discover names. The name matches exactly, else as a unique case-insensitive substring; a miss or an ambiguity reports the frame's candidate names. Textures support depth (grayscale), float (normalized), packed and color formats — adjust rangeMin/rangeMax for float/depth visualization. No transfer curve is applied, so the pixel values are the ones the graph holds. Scene colour textures (the view's colour target, the render graph's preExposed textures, and the scene-colour textures passes create: TAA, SSR, depth of field, the ocean overlays, the scene grabs, a fullscreen pass's history and ping-pong chain) are stored at the frame's pre-exposure: the response reports preExposed and preExposure, the PNG maps scene-linear values (texel / preExposure), and raw texels are returned as stored, to be divided by preExposure.",
    schema: {
      name: z.string().describe("Render graph resource name (e.g. 'SceneView.Color', 'SceneView.Depth')"),
      rangeMin: z.coerce.number().optional().describe("Textures: value mapped to black (default 0.0). For depth: near plane value."),
      rangeMax: z.coerce.number().optional().describe("Textures: value mapped to white (default 1.0). For depth: far plane value."),
      mip: z.coerce.number().optional().describe("Textures: mip level to capture (default 0)"),
      layer: z.coerce.number().optional().describe("Textures: array layer / cube face to capture (default 0)"),
      offset: z.coerce.number().optional().describe("Buffers: byte offset to start reading (default 0)"),
      byteCount: z.coerce.number().optional().describe("Buffers: bytes to read (default: to the end, capped at 1 MiB)"),
      decode: z.enum(["hex", "float", "uint"]).optional().describe("Buffers: also return the bytes decoded as an array of float32 or uint32 (default hex = base64 only)"),
    },
    run: async (args) => {
      const params: Record<string, unknown> = {};
      for (const [k, v] of Object.entries(args)) if (v !== undefined) params[k] = v;
      const result = await editorCall("capture_resource", params) as any;
      if (result?.pngBase64) {
        const { image, data } = splitImage(result, true);
        return {
          data, image,
          summary: `Captured ${result.resourceName} (${result.width}x${result.height}, ${result.format}, ${result.channels}ch, mip ${result.mip} layer ${result.layer}) → ${result.filePath}`,
        };
      }
      if (result?.kind === "buffer") {
        const values = result.floats ?? result.uints;
        const preview = values ? `\n${JSON.stringify(values.slice(0, 64))}${values.length > 64 ? ` … (${values.length} total)` : ""}` : "";
        return {
          data: result,
          summary: `Captured buffer ${result.resourceName} (${result.byteCount} bytes @ offset ${result.offset})${preview}`,
        };
      }
      return { data: result };
    },
  }),

  defineTool({
    name: "trigger_capture",
    category: "capture",
    ipcMethod: "trigger_capture",
    description: "Trigger a RenderDoc frame capture and return the record line: the .rdc path, its size in bytes, its SHA-256, the scene view camera pose and the graphics validation counts. The editor must be running under RenderDoc — launch_editor with renderdoc=true does that. Waits for the capture to finish writing (a large scene serializes >100 MB over many frames). The .rdc lands at the editor's capture path template: GE_RENDERDOC_CAPTURE_DIR if set, else <editor exe dir>/Captures/editor_frameN.rdc. When RenderDoc is not attached the call fails with an error naming the reason. If the wait times out, the capture is still completing: call get_capture_status to recover its path.",
    schema: {},
    run: async () => {
      const result = await editorCall("trigger_capture", {}) as any;
      if (!result?.filePath) return { data: result };
      const record = await recordFromFile(result, String(result.filePath));
      return {
        data: { captured: result.captured, captureIndex: result.captureIndex, ...record },
        summary: formatCaptureRecord(`RenderDoc capture ${result.captureIndex}`, record),
      };
    },
  }),

  defineTool({
    name: "get_capture_status",
    category: "capture",
    ipcMethod: "get_capture_status",
    description: "Report RenderDoc capture state immediately: whether RenderDoc is attached, the capture count, and the record line of the newest capture (path, size in bytes, SHA-256). Use it to recover a capture whose trigger_capture call timed out before RenderDoc finished writing the .rdc, and to check whether the editor is running under RenderDoc at all before triggering.",
    schema: {},
    run: async () => {
      const result = await editorCall("get_capture_status", {}) as any;
      if (!result?.available) {
        return { data: result, summary: "RenderDoc is not attached: no captures. launch_editor renderdoc=true starts the editor under RenderDoc." };
      }
      if (!result.lastCapturePath) {
        return { data: result, summary: `RenderDoc attached; ${result.captureCount} capture(s); none on disk yet.` };
      }
      const record = await recordFromFile({}, String(result.lastCapturePath));
      return {
        data: { available: true, captureCount: result.captureCount, ...record },
        summary: `RenderDoc attached; ${result.captureCount} capture(s). ${formatCaptureRecord("Newest", record)}`,
      };
    },
  }),

  proxyTool({
    name: "nsight_capture",
    category: "capture",
    description: "Trigger an NVIDIA Nsight Graphics frame capture. The editor must have been launched by ngfx-capture.exe (use mcp/scripts/ngfx-capture-editor.mjs) — the capture libraries arrive by injection, so an editor started normally cannot capture. GE_VK_CAPTURE_COMPAT=1 is recommended so VK_EXT_descriptor_buffer is out of the capture's way; the launch script sets it by default. Note that get_gpu_tooling does NOT confirm Nsight: it injects by loader interception rather than registering a Vulkan layer, so attachedTools stays empty even while capture works. The error this tool fails with is the availability answer; to check injection out-of-band, look for ngfx-capture-interception.dll in the editor's loaded modules. Returns the capture file path once Nsight finishes serializing it, which takes noticeably longer than a RenderDoc capture — raise GE_MCP_IPC_TIMEOUT_MS (default 30000) if a heavy scene or a large frames count outruns the client's wall-clock timeout.",
    schema: {
      frames: z.coerce.number().int().min(1).max(60).optional()
        .describe("Number of consecutive frames to capture, starting at the next present (default 1, max 60)."),
    },
  }),
];
