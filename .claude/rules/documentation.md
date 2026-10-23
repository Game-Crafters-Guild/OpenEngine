---
paths:
  - "docs/**"
---

# Documentation under `docs/`

`docs/` holds what a user of the engine reads to use a feature, write an authored format, or understand a concept. Development documentation (architecture proposals, designs, decisions, investigations, measurements, plans, post-mortems, review records) lives in the workbench repository (`GameEngine-Workbench/designs/`, `measurements/`, `notes/`) and is cited from code by name only; the code comment carries the invariant or trap itself. A design a user must read to use the feature is a usage guide that has not been written yet: write the guide here and keep the design in the workbench.

openengine.dev will publish the pages under `/docs/`, rendered by the site's Starlight renderer, once the site switches to them; github.com renders the same files in the public repository. Every page is in the form below, in its area folder; `docs/README.md` is the github.com index of the areas.

## Format

A page is Markdown with directives, in an `.mdx` file. No per-page styling, no inline `<style>` or `<script>`, no `import` lines and no JSX: the site's stylesheet and components do the presentation.

- **Front matter** on every page: `title` (the page heading; the renderer fails the build without it) and `description` (one sentence, shown in search and link previews), and `sidebar.order` where the page's place in its area matters.
- **Sentence case** for titles and headings (`The input system`, `Camera framing`); panel, window and product names keep the case the editor shows (`Scene View`, `AI Assistant`).
- **Area folders**, lower case, one per sidebar group: `docs/getting-started/`, `editor/`, `scripting/`, `rendering/`, `terrain/`, `input/`, `physics/`, `animation/`, `assets/`, `navigation/`, `audio/`, `ui/`, `packages/`, `publishing/`, `contributing/`, and the API reference under `docs/reference/<module>/`. File names are lower case with hyphens (`input-system.mdx`); the path is the page's address (`docs/input/input-system.mdx` is `/docs/input/input-system/`). A new page in an area folder appears in that area's sidebar group; nothing is edited by hand. A new area adds its line to `docs/README.md` and its entry to the site's area list (`kAreas` in the site's `src/markdown/docs-sidebar.mjs`). No two folders under `docs/` differ only in case; area folders are lower case.
- **Links** are file links relative to the page, with the extension and the anchor: `[Pointer lock](../input/input-system.mdx#pointer-lock)`. github.com follows them as written and the site rewrites them to the page's address; a link to a page or a heading that does not exist fails the site build. No site-relative (`/docs/...`) or directory links.
- **Code** in fenced blocks with the language, and every fence except `mermaid` with a title: a declaration fence is titled with its header or assembly (```` ```cpp title="Input/InputSystem.h" ````), an example fence with what it shows (```` ```cpp title="Jump on space, fire while the left button is held" ````). A diagram is a ```` ```mermaid ```` fence; a diagram's label becomes an italic caption directly under its fence, as an image's does, and a Mermaid label that holds parentheses, brackets or braces is quoted: `-->|"Compile()"|`.
- A root file (`BUILD.md`, `CodingStyle.md`, `CONTRIBUTING.md`) is linked by its public repository URL, `https://github.com/Game-Crafters-Guild/OpenEngine/blob/main/<file>`: the site publishes `docs/` only.
- A source, shader or asset file is named as a code path from the repository root, not linked (the site publishes pages only); a table under a folder heading may give paths relative to that folder.
- Write `file:line`-style text and ratios as code (`` `1:1` ``) or escape the colon (`1\:1`): a colon before a letter or digit opens a text directive.
- A measured time or cost names the content, hardware, build configuration and date, or is dropped or rounded to an order of magnitude; a size computed from the format needs no configuration.
- Exponents: `^` inside a code formula; Unicode superscripts for units and small powers in prose (km², 8192²); e-notation for very large or very small numbers (3.4e38).
- No `---` thematic breaks: headings separate sections (a `---` under a paragraph line turns that line into a heading).
- **C++ and C#.** Every API item that has both forms is one tabbed code view, `C++` first, labels exact. A side with no binding holds a `C# binding pending` caution naming the tracking issue and the native call, never an empty tab:

  ````markdown
  :::::tabs
  ::::tab[C++]

  ```cpp title="Input/InputSystem.h"
  Mathematics::Vector2 GetMouseDelta() const;
  ```

  ::::
  ::::tab[C#]

  :::caution[C# binding pending]
  No managed call yet; tracked as [OpenEngine issue #14](https://github.com/Game-Crafters-Guild/OpenEngine/issues/14).
  :::

  ::::
  :::::
  ````

- **Directives.** An outer container carries more colons than any container inside it (five for the tab set, four for a tab, three for an aside in a tab): a nested container with the same count closes the outer one. Leave a blank line after the opening line and before the closing line of a container that holds a code block, a list or a table.
  - No directive inside a heading: it changes the anchor. A badge goes on the line under the heading or in the entry's `:::meta`.
  - A card grid holds short parallel text (a paragraph per card, no lists); longer parallel content is sibling headings.
  - No HTML comments; a site insertion point is a leaf directive (`::name`) the site plugin handles.

  | Directive | Renders as |
  |---|---|
  | `:::note`, `:::tip`, `:::caution`, `:::danger`, optionally `[Title]` | an aside |
  | `:::::tabs` holding `::::tab[C++]` and `::::tab[C#]` | a code view with a language strip; one click switches every view on the page and the choice holds across the site |
  | `::::cardgrid` holding `:::card[Title]` | cards side by side (before and after, two options) |
  | `:::card[Title]{href=../area/page.mdx#anchor}` | a linked card (the reference entries a section used) |
  | `:::steps` around an ordered list | numbered steps |
  | `:badge[New]{variant=success}` (`danger`, `caution`, `note`) | an inline status badge |
  | `:::meta` around a list of `- **Key**: value` lines | a reference entry's metadata list |

- A badge is for a status only (supported, untested, deferred, new, removed). A class, token or identifier is inline code, and whether something is required or optional is plain text in the table's Required column.
- An aside or card title goes in brackets; an aside with nothing to title stays untitled, and no title is invented.
- A formula that stands as its own paragraph is a `text` fence titled with what it computes; inline code is for a formula inside a sentence.
- A token or color list is a `Token | Value | Use` table.
- A figure is the image, then its caption as an italic paragraph directly under it; the alt text says what the image shows.
- Editor UI is shown by a captured screenshot in `img/`, never by a hand-drawn mock.
- A page describes the feature as it is now. No changelog section, no "was" or "previously", no comparison with an earlier version: a user reads what the engine does today. A behavior change on authored data is stated once, in the release changelog and in the pull request body, never on the page.
- **Images** live in an `img/` folder beside the pages of their area (`docs/editor/img/ai-assistant-panel.png`, referenced as `img/ai-assistant-panel.png`), named `<page>-<what>.png`, never beside the page itself. The root `README.md`'s images live in `docs/images/`. User-facing screenshots are captured at a normal window size (1920x1080 class), through the render-graph readback, and cropped to the panel they show.
- American spelling. No other engine's name in user-facing text; import features keep their names.
- **Prose** follows the `writing-simplified-technical-english` skill: one idea per sentence, short sentences, active voice, no semicolons or em dashes, contractions expanded, and the Oxford comma. Code spans, fenced blocks, link targets and what a heading means stay as they are. A page that does not fit is edited toward it on contact, never in unprompted bulk.
- The root files (`README.md`, `BUILD.md`, `CodingStyle.md`, `CLAUDE.md`, `AGENTS.md`, `CONTRIBUTING.md`) stay plain Markdown at the root by convention.

## One canonical document per topic

- Designs, specs, roadmaps and guides are living documents: one canonical file per topic, named without a date (`mesh-lod-usage-guide.mdx`, not `mesh-lod-usage-guide-2026-07-12.mdx`). When implementation deviates or the thinking moves, amend the canonical; never open a dated sibling.
- A new file requires a genuinely new topic. If a lane must draft in a separate dated file (parallel worktrees, an exploratory rewrite), that file is folded into the canonical and deleted before the merge finisher signs off.
- Records of a moment stay dated and immutable: investigations, post-mortems, measurement and attribution reports, review verdicts, go/no-go decisions, pre-registrations. A reversed verdict is a new record plus a correction banner in the old one.
- Supersession is explicit and two-sided: the successor names the file it replaces; the replaced file gets a banner pointing forward or is deleted in the same change. A moved or converted page updates every link to it in the same change. "Extends" is not a relationship.
- Arc close is consolidation: fold the arc's dated iterations into canonicals and delete them; commit or delete every untracked document the arc produced (a tracked document citing an untracked one is a defect); flip status lines (ACTIVE → CLOSED); log review verdicts in `GameEngine-Workbench/review-tier-log/`.
- Migrate on contact: when touching a pre-policy dated canonical, rename it undated as part of the change. No bulk renames.
