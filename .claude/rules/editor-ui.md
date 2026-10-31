---
paths:
  - "Apps/Editor/**"
  - "Engine/Modules/UI/**"
  - "Engine/Modules/EditorSDK/**"
  - "**/*.css"
  - "**/*.uxml"
---

# Editor and UI

## Declared, not assembled

- Declare static layout in a `.uxml` in the owning folder or with `UI::Element<T>` in the owning C++ UI implementation. The typed DSL applies to any C++-defined UI structure. Style belongs in the owning UI's `.css` file, loaded by its control; behavior goes on the control or a manipulator. Compose existing shared controls and keep presentation separate from application logic. Hard-coded geometry, ad hoc rows or copies of shared widgets are defects in either authoring format. A composite widget is its own `UIElement` class.
- No styling from C++, no colours in code; colours come from CSS variables; icons are assigned by class, not by a C++ map. The shared style is the default: when every call site adds the same classes, fold them into the stylesheet or add one modifier class; style a category with one shared class, never one class per item; fix the art asset instead of per-item pixel compensation.
- One selector per CSS rule. No selector groups, no ancestor chains, no `button.x` / `.button.x` / `.x` variants, no `:checked` repeated as `.checked`. Several elements that share a style share one class, set at the C++ creation site. This is the single most repeated review comment in the repository's history. The cascade is specificity first, then last wins; sweep for cascade regressions.
- No font size below 12 px in the editor unless the owner asks for it.
- Drags emit value-changing; commit on value-changed. A field that must not propagate stops propagation itself. Popup lifecycle lives in a base class with one hook. For interaction semantics, match Chrome and the CSS specification.
- Generic stays generic: a base class carries only what every subtype uses; engine UI core never special-cases a control type; a view does not know whether it is "primary" or in play mode.

- A control is a class on the UI control base with its layout declared in XML or the typed C++ DSL and its styling in CSS, placed in the module that owns its use under that module's UI folder (`Controls`, `Inspectors`, `Panels` as the module needs) and registered through the registries. No imperative geometry inside a panel, no copies of shared controls assembled in place, no control filed under a hub. A control two modules share lives in the UI module's controls.

## Understood without explanation

Every surface a user sees or operates (a panel, an inspector section, a setting, a dialog, a menu, a notice, a control, user-facing text) is understood without explanation. A control that works but must be explained is a defect. The checklist is the UI/UX pass in the `change-scrutiny` skill (`.claude/skills/change-scrutiny/SKILL.md`); a change runs it before its pull request opens, and the body carries its line.

## Registration, hubs and ownership

- New settings categories and fields go through `Editor::EditorSettingsRegistry` (EditorSDK; usable from engine systems and packages) and the `SettingsPanel` renders them from descriptors. Never a hand-built page in `SettingsPanel.cpp`; migrate legacy pages on contact. Persist through `SettingsStore`, never raw JSON.
- Inspectors, post effects, search items and dockables register with their registries, never a hard-coded list. Never teach `InspectorPanel` about a manager: write a custom inspector. Never pass a panel into a controller; a command builds its own state in its constructor; integrations subscribe to events rather than living in the emitter.
- A file lives in the folder of its owner (`SceneView/Gizmos`, the VCS module, a control's own folder); a feature with more than one file gets a folder named for it (`Panels/ProjectPicker/` holds the picker's modal, its cards and its helpers; its layouts and sheets sit under `UI/controls/ProjectPicker/`), and a new file joins that folder or starts one; a flat folder of unrelated features (`Panels/` with sixty files) is a cleanup class, never a home for one more; the `Editor/` root folder takes nothing new. Platform utilities are free functions in the Platform module. Effects only some users need are a package, not core rendering.
- Drawing goes through the gizmo API (`Apps/Editor/Include/SceneView/SceneViewGizmos.h`: `DrawLine`, `DrawWireBox`, `DrawWireSphere`, `DrawWireCircle`, line batches); pickers are UI controls; every shared control lives under `Engine/Modules/UI/Include/UI/Controls`.

## The two asset mounts

The running editor resolves the same logical path against two mounts:

- **`editor`**: `<Editor.exe dir>/Assets/`, staged from `Apps/Editor/Assets/` by the build. Editor chrome: icons, themes, default render graphs, default UI XML, default scenes. The staged copy is what the editor reads and hot-reload watches: to live-edit editor assets, edit `build/<preset>/bin/<config>/Apps/Editor/Assets/...`, and mind the configuration (editing under `Debug/` while the editor runs `DebugFast/` does nothing).
- **`project`**: the open project directory (`--project <path>`, or the last project from preferences). User content. Never the repository root; use `Apps/Editor/Player/` or a dedicated test project such as `Tests/EditorHarness/rg2-gate1-content/`.

With no `--project` the project root falls back to the executable directory and the mounts overlap; tests that depend on which mount wins set up distinct directories. Unprefixed paths resolve project-first, then editor; CSS pins a mount with `url("editor:Icons/foo.png")` or `url("@editor/Icons/foo.png")`. `node mcp/ge.mjs get_build_settings` prints the workspace root and resolved asset root.

## Previews and commits

A field previews while the user types or drags. A preview is a function of the value the edit started from and the current input, never of the previous preview: typing 70 over 75 passes through 7. A setter that also adjusts another field (a clamp that depends on the edited value) applies that adjustment once, at commit; a row that applies it on every keystroke destroys the dependent field's stored value. A committed field shows what the component stores, with its caption.

## Live editor checks

An editor for verification runs from an isolated copy of the staged tree against a copy of the project, on a port verified free (never 9999, the owner's, nor 9998, the renderer stream), with `GE_FORCE_HDR_OUTPUT=Off`, and is stopped by its recorded process id, never by name. The Game View tab gates managed UI reads: with the Scene View active, HUD reads are dead while the HUD renders. Screenshots report their own method; assert `method == "rendergraph"`. `get_ui_tree` is depth-limited; deep popups need a screenshot crop.

The scene view's default hover mode needs Control held: send it through the debug server's `send_key`. A capture whose UI tree readback shows a hovered element the script did not place is rejected: a stray hover changes the colours being judged.
