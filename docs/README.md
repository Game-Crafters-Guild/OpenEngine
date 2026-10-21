# Documentation

The engine's documentation, published on [openengine.dev/docs](https://openengine.dev/docs/) and readable here on github.com. Each page is an `.mdx` file in its area's folder. The site's sidebar lists the areas in this order:

- [`getting-started/`](getting-started/): a first project, running the Player headless, the MCP server and CLI
- [`editor/`](editor/): the editor's panels and tools
- [`scripting/`](scripting/): C# and native C++ scripting, the ECS from code, paths, settings, and memory
- [`rendering/`](rendering/): the renderer, its effects, materials, and render pipelines
- [`terrain/`](terrain/): terrain detail, shadows, heightmaps, and grass
- [`input/`](input/): the input system
- [`physics/`](physics/): physics events
- [`animation/`](animation/): retargeting, clip events, the glTF clip schema, and animation sources
- [`assets/`](assets/): model extras, Unity import, texture compression, alpha coverage, and smart folders
- [`navigation/`](navigation/): the navigation surface
- [`audio/`](audio/): playing sounds, the audio formats a project can import, and the audio mute
- [`ui/`](ui/): the UI API, game UI scaling, control defaults, text effects, and transforms
- [`packages/`](packages/): the package data model
- [`publishing/`](publishing/): game builds, Steam publishing, and the desktop Player's sources
- [`contributing/`](contributing/): material for engine contributors, from the scripting ABI to editor guidelines

Code that exists in both C++ and C# is one view with a `C++` tab and a `C#` tab. The page form (front matter, links, directives, images) is in [`.claude/rules/documentation.md`](../.claude/rules/documentation.md). Building the engine is in [`BUILD.md`](../BUILD.md), and contributing in [`CONTRIBUTING.md`](../CONTRIBUTING.md).
