# miniz (vendored)

- Upstream: https://github.com/richgel999/miniz
- Version: 3.0.2 (tag `3.0.2`)
- License: MIT (see `LICENSE`)
- Files: the split sources from the repo root (`miniz*.c/.h`), not the
  release amalgamation.

Built as the `EditorMiniz` static library behind the EditorSDK's zip reader
(`Editor/ZipArchiveReader.h`), which project acquisition and Editor-kind package
modules use to read `.zip` archives. Vendored rather than pulled
via vcpkg because it's a self-contained single-purpose C library and the
repo's worktree flow reuses a shared read-only vcpkg tree; swap to the vcpkg
`miniz` port if that trade-off changes.

Update procedure: copy the same file set from a tagged upstream checkout,
update the version here, and rebuild `EditorSDK` and `ZipArchiveReaderTests`.
