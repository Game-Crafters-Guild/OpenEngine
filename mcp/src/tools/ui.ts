import { z } from "zod";
import type { ToolDef } from "../registry.js";
import { definedOnly, proxyTool } from "./proxy.js";

// Mirrors kDefaultUITreeMaxTextLength (Apps/Editor/Source/DebugServer/UITreeDump.h). The editor
// applies its own default when the parameter is absent; this keeps the two tools that advertise it
// from drifting apart from each other.
const kDefaultMaxTextLength = 512;
const kMaxTextLengthHelp =
  `Cut text values longer than this (default ${kDefaultMaxTextLength}; 0 = whole text). A cut node ` +
  `carries textTruncated:true, so a substring you do not find is genuinely absent unless that flag is set`;

// GLFW key codes start at 32, so an integer from 0 to 9 names no key: send_key reads it as the
// digit key. The CLI passes the token of `--key 8` as the number 8.
function digitKeyAsName(key: string | number): string | number {
  return typeof key === "number" && Number.isInteger(key) && key >= 0 && key <= 9 ? String(key) : key;
}

// GLFW key codes run from 32 to 348 (GLFW_KEY_LAST). The schema refuses an integer that is
// neither a digit nor a GLFW key code.
function namesAKey(key: string | number): boolean {
  return typeof key !== "number" || (key >= 0 && key <= 9) || (key >= 32 && key <= 348);
}

function noKeyMessage(key: string | number): { message: string } {
  return { message: `key ${key} names no key: key takes a key name, a digit 0 to 9, or a GLFW key code from 32 to 348` };
}

export const tools: ToolDef[] = [
  proxyTool({
    name: "get_ui_tree",
    category: "ui",
    description:
      "Get the editor UI element tree for debugging the editor interface. Two defaults cut the walk: maxDepth (default 5), far above most panel content — the scene-view toolbar sits below depth 12 — and includeHidden (default false), which drops hidden subtrees entirely. So a dump that does not contain an id does NOT mean the id is absent. Read searchIsComplete in the reply: absence only means something when it is true, and completenessNote says which limit applied. To look one id up regardless of depth, pass it as rootId, which searches the whole tree.",
    schema: {
      maxDepth: z.coerce.number().optional().describe("Max tree depth (default 5). Raise it, or use rootId, before concluding an element is missing"),
      rootId: z.string().optional().describe("Start from this element ID. Found by an unbounded search, so this locates elements at any depth"),
      includeLayout: z.coerce.boolean().optional().describe("Include x/y/w/h layout data (default true)"),
      includeHidden: z.coerce.boolean().optional().describe("Include hidden (display:none) elements, marked with hidden:true (default false)"),
      maxTextLength: z.coerce.number().optional().describe(kMaxTextLengthHelp),
    },
    params: ({ maxDepth, rootId, includeLayout, includeHidden, maxTextLength }) => ({
      maxDepth: maxDepth ?? 5,
      rootId,
      includeLayout: includeLayout ?? true,
      includeHidden: includeHidden ?? false,
      maxTextLength: maxTextLength ?? kDefaultMaxTextLength,
    }),
  }),

  proxyTool({
    name: "get_panel_tree",
    category: "ui",
    description:
      "Get a docked panel's UI subtree by panel name (e.g. 'Hierarchy', 'Inspector', 'Assets'). Unlike get_ui_tree with rootId, this resolves the actual rendered panel content regardless of its position in the dock layout. Same completeness contract as get_ui_tree: the reply carries searchIsComplete, and absence of an id means nothing unless it is true — this walk is cut both by maxDepth and by the default that omits hidden subtrees.",
    schema: {
      panelId: z.string().describe("Panel name (e.g. 'Hierarchy', 'Inspector', 'Assets', 'SceneView')"),
      maxDepth: z.coerce.number().optional().describe("Max tree depth (default 5). Raise it before concluding an element is missing"),
      includeLayout: z.coerce.boolean().optional().describe("Include x/y/w/h layout data (default true)"),
      includeHidden: z.coerce.boolean().optional().describe("Include hidden (display:none) elements, marked with hidden:true (default false)"),
      maxTextLength: z.coerce.number().optional().describe(kMaxTextLengthHelp),
    },
    params: ({ panelId, maxDepth, includeLayout, includeHidden, maxTextLength }) => ({
      panelId,
      maxDepth: maxDepth ?? 5,
      includeLayout: includeLayout ?? true,
      includeHidden: includeHidden ?? false,
      maxTextLength: maxTextLength ?? kDefaultMaxTextLength,
    }),
  }),

  proxyTool({
    name: "click_element",
    category: "ui",
    description:
      "Click a UI element by ID or by UI coordinates. Takes the same route real mouse input does, so in play mode with a Game View focused it also reaches the running game's input (position remapped to viewport-local). 'clicked' only reports that pointer input was injected at the returned coordinates. 'hit' names the element the press resolved onto, as information rather than a verdict: it is sampled after the press, so on a click that changes UI state it is nondeterministic and can come back different or empty for a click that worked. Treat a mismatch as a reason to inspect, never as proof the click missed. An elementId whose layout box has collapsed on both axes (hidden, not yet laid out, or a host whose content is mounted under another id) is refused rather than aimed at its origin.",
    schema: {
      elementId: z.string().optional().describe("Element ID to click. Must have a layout box with extent on at least one axis"),
      x: z.coerce.number().optional().describe("X in the editor window's UI coordinates (the logical pixels get_ui_tree reports), not desktop pixels"),
      y: z.coerce.number().optional().describe("Y in the editor window's UI coordinates (the logical pixels get_ui_tree reports), not desktop pixels"),
      button: z.enum(["left", "right", "middle"]).optional().describe("Mouse button (default left). right opens real context menus — on macOS the popup is deferred past the IPC reply, so this does not stall; on Windows the native popup's nested message loop blocks the debug server until the menu closes, so prefer open_context_menu there"),
      clickCount: z.coerce.number().int().min(1).max(3).optional().describe("Clicks in one gesture (default 1): 2 is a double-click, 3 a triple-click. The presses land inside the platform's double-click interval at any frame rate, which two separate click_element calls cannot promise"),
    },
  }),

  proxyTool({
    name: "perform_drop",
    category: "ui",
    description:
      "Drop asset files at a point in an editor window, as a drag from the Assets panel would end there. The window's drag-and-drop manager resolves the drop target under the point (the Scene View viewport, the Hierarchy tree, an inspector field), and that target's own CanDrop and PerformDrop run, so it exercises the same code a drop with the mouse does, without a pointer gesture to start the drag. 'accepted' says the target took the payload and its PerformDrop ran; PerformDrop reports no result, so read the effect (get_scene_hierarchy, get_undo_stack, the Assets panel). 'reason' says why the target did not take it; 'target' names the drop target element. Refused before any drop: a path outside the project's asset sources (the Assets panel's drop moves files, so copy an outside file into the project first), a path with no file, and a drag-and-drop session in progress. Moves the editor's pointer to the point (the desktop cursor stays where it is). A drop the target defers until an asset loads returns at once with accepted true; read the result later.",
    schema: {
      paths: z.array(z.string()).min(1).describe("Asset files to drop, absolute or project-relative; each must exist inside one of the project's asset sources"),
      elementId: z.string().optional().describe("Drop at this element's center, e.g. SceneViewViewport. Must have a layout box"),
      x: z.coerce.number().optional().describe("X in the editor window's UI coordinates (the logical pixels get_ui_tree reports), not desktop pixels"),
      y: z.coerce.number().optional().describe("Y in the editor window's UI coordinates (the logical pixels get_ui_tree reports), not desktop pixels"),
      windowIndex: z.coerce.number().optional().describe("Editor window (default 0, the main window)"),
    },
  }),

  proxyTool({
    name: "move_pointer",
    category: "ui",
    description:
      "Move the editor's pointer without clicking and report the resolved hover target, for hover-state testing and screenshots. Only the editor's pointer moves: the desktop cursor stays where it is. Takes the same route real mouse input does, so in play mode with a Game View focused it also drives the running game's mouse (position remapped to viewport-local). An elementId whose layout box has collapsed on both axes (hidden, not yet laid out, or a host whose content is mounted under another id) is refused rather than aimed at its origin.",
    schema: {
      elementId: z.string().optional().describe("Element ID whose center should be hovered. Must have a layout box with extent on at least one axis"),
      x: z.coerce.number().optional().describe("X in the editor window's UI coordinates (the logical pixels get_ui_tree reports), not desktop pixels"),
      y: z.coerce.number().optional().describe("Y in the editor window's UI coordinates (the logical pixels get_ui_tree reports), not desktop pixels"),
    },
  }),

  proxyTool({
    name: "simulate_mouse_drag",
    category: "ui",
    description:
      "Press, move and release the editor's pointer across frames, as a mouse drag does: controls that arm on the press, drag on later moves and commit on the release see the same events. The pointer moves from the start along the path points and then to endX/endY, one move per presented frame, and releases on the last point. steps moves are spread over the segments by length, and every segment ends exactly on its point, so a path traces a freehand stroke: with the spline tool, a path that comes back to its start closes the loop. Only the editor's pointer moves: the desktop cursor stays where it is. dragDropSession says whether the gesture started a drag-and-drop session; perform_drop makes a drop without one.",
    schema: {
      elementId: z.string().optional().describe("Start at this element's center. Must have a layout box"),
      startX: z.coerce.number().optional().describe("Start X in the editor window's UI coordinates (the logical pixels get_ui_tree reports)"),
      startY: z.coerce.number().optional().describe("Start Y in the editor window's UI coordinates"),
      path: z.array(z.object({ x: z.coerce.number(), y: z.coerce.number() })).optional().describe("Points the pointer passes through after the start, in UI coordinates, in order (at most 240). Give path, endX/endY or both; the release is on the last point"),
      endX: z.coerce.number().optional().describe("End X in UI coordinates, after the path"),
      endY: z.coerce.number().optional().describe("End Y in UI coordinates, after the path"),
      steps: z.coerce.number().int().optional().describe("Pointer moves over the whole drag, one per frame (default 20, at most 240; never fewer than the path's segments)"),
      button: z.enum(["left", "right", "middle"]).optional().describe("Mouse button (default left)"),
      holdFrames: z.coerce.number().int().optional().describe("Frames the button stays down at the start before the first move (default 1, at most 4)"),
      holdBeforeReleaseFrames: z.coerce.number().int().optional().describe("Frames the button stays down on the last point before the release (default 0, at most 1200)"),
      captureAtFrame: z.coerce.number().int().optional().describe("Take a window capture at this hold frame (0 to holdBeforeReleaseFrames - 1); the reply carries its file and the pointer state of that frame"),
      windowIndex: z.coerce.number().optional().describe("Editor window (default 0, the main window)"),
    },
  }),

  proxyTool({
    name: "send_scroll",
    category: "ui",
    description:
      "Turn the mouse wheel in the editor, along the path a real wheel takes: the editor UI, a game HUD, then the game's input in play mode (a wheel zoom over the Game View). With a target, the pointer moves there and the wheel starts on the next frame; without one, the wheel turns at the pointer. count puts the delta on that many frames, one delta per frame, as a wheel turned over several frames does. The reply follows the last delta and names the element under the pointer. Modifiers travel with the wheel as the platform reports them, so mods=control is the Ctrl+wheel item resize gesture (Assets grid icon size) with no key press first. Only the editor's pointer moves: the desktop cursor stays where it is.",
    schema: {
      elementId: z.string().optional().describe("Element ID whose center the wheel turns over. Must have a layout box with extent on at least one axis"),
      x: z.coerce.number().optional().describe("X in the editor window's UI coordinates (the logical pixels get_ui_tree reports), not desktop pixels. Give x and y together"),
      y: z.coerce.number().optional().describe("Y in the editor window's UI coordinates (the logical pixels get_ui_tree reports), not desktop pixels. Give x and y together"),
      dy: z.coerce.number().optional().describe("Vertical wheel steps per frame (one notch of the wheel is 1); positive is a wheel up (away from the user). Default 0"),
      dx: z.coerce.number().optional().describe("Horizontal wheel steps per frame (one notch of the wheel is 1). Default 0; dx or dy must be non-zero"),
      count: z.coerce.number().int().optional().describe("Frames that each take the dx/dy delta (default 1, at most 240)"),
      mods: z.union([z.string(), z.array(z.string())]).optional().describe("Modifier(s) held during the wheel: shift | control | alt | super (string or array)"),
      windowIndex: z.coerce.number().optional().describe("Editor window (default 0, the main window)"),
    },
  }),

  proxyTool({
    name: "send_key",
    category: "ui",
    description: "Inject a keyboard event into the editor along the path a real key takes: global editor shortcuts first, then the focused element, then a running game's HUD and input, then editor actions. Use it for keyboard-driven behavior like Tab focus traversal, Enter commit and Escape cancel, and for typing into a game HUD while play mode runs. Pair with take_screenshot to observe focus highlights.",
    schema: {
      key: z.union([z.string(), z.number()]).refine(namesAKey, noKeyMessage).describe("Key name (Tab, Enter, Return, NumpadEnter, Escape, Space, Backspace, Delete, Home, End, Up, Down, Left, Right, A-Z, 0-9) or a raw GLFW keycode integer (32 to 348); an integer from 0 to 9 is the digit key, and any other integer is refused"),
      mods: z.union([z.string(), z.array(z.string())]).optional().describe("Modifier(s): shift | control | alt | super (string or array). Shift+Tab tabs backwards."),
      action: z.enum(["tap", "press", "release", "repeat"]).optional().describe("tap (default: press + release next frame), press, release, or repeat (OS key-repeat edge, e.g. held Tab)"),
      count: z.coerce.number().optional().describe("tap only: number of taps to issue (e.g. Tab x3), 1 to 64. Each tap is a press on one frame and a release on the next, so a game reads each tap as its own press; the reply comes after the last release. Default 1."),
    },
    params: (args) => definedOnly({ ...args, key: digitKeyAsName(args.key) }),
  }),

  proxyTool({
    name: "input_text",
    category: "ui",
    description: "Type text into the focused editor field through the path real typing takes: one character event per codepoint, so the field's per-keystroke callbacks run as they do for a user. Pass elementId to focus a field first (the same focus a click gives it). Refused before anything is typed: text that is not valid UTF-8, or that holds a control character (use commit for Enter and send_key for Tab or Escape), and a call with no field focused. The keystrokes are applied on the editor's next frame.",
    schema: {
      text: z.string().describe("The text to type (UTF-8, no control characters)"),
      elementId: z.string().optional().describe("Element to focus before typing; omitted, the text goes to the focused element"),
      clear: z.coerce.boolean().optional().describe("Select the field's content first so the text replaces it (default false)"),
      commit: z.coerce.boolean().optional().describe("Press Enter after the text, which commits numeric fields and runs Enter-bound actions (default false)"),
      windowIndex: z.coerce.number().optional().describe("Editor window (default 0, the main window)"),
    },
  }),
];
