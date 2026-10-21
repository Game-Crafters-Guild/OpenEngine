# fbtBlend (Vendored)

Vendored copy of the fbtBlend-Header-Only library by Flix01.

## Upstream

- Repository: https://github.com/Flix01/fbtBlend-Header-Only
- License: Zlib (see `LICENSE` next to this file)
- Commit pinned: `ddb0d3616f4f5ee7b45f62647b269f56f888816d`
- Commit date: 2026-03-19
- Commit message: "Added Blender 5.1 support in folder 501."

## Files vendored

| File | Source path in upstream | Bytes |
|---|---|---|
| `fbtBlend.h` | `405_LTS/fbtBlend.h` (with the local modifications below) | 548 977 upstream |
| `Blender.h`  | `405_LTS/Blender.h` (with the local modifications below) | 221 515 upstream |
| `LICENSE`    | `LICENSE`            |     900 |
| `fbtBlend.cpp` | (added by us — implementation shim) | ~2.6k |

## Blender version coverage

fbtBlend's `405_LTS/` folder targets Blender 4.0.x through 4.5 LTS.
This is the right tier for our current sample bundles (Blender 4.0.0,
4.0.2). It uses the OLD 12-byte `.blend` header format (`'BLENDER-v405'`
style); the upstream `501/` folder (Blender 5.x) introduced an
INCOMPATIBLE 17-byte header format that rejects every Blender 4.x file
as "Unknown header format version". A binary that needs both Blender
4.x and 5.x files would have to compile two TUs (one per folder) and
dispatch on the file header — captured as a Phase B5+ follow-up; not
needed for the user's three sample bundles.

Once the user provides a Blender 5.x sample, switch to a dual-TU build
(or to the next upstream commit that unifies both header readers).
**Currently-vendored coverage: Blender 2.79 (via SDNA fallback) to
4.5 LTS. Blender 5.0 / 5.1 files are NOT supported.**

## Compression dependencies

- **zlib** (`FBT_USE_GZ_FILE=1`) is required for `.blend` files saved by
  Blender < 3.0 (PM_COMPRESSED, gzip).
- **zstd** (`FBT_USE_ZSTD_FILE=1`) is required for `.blend` files saved
  by Blender >= 3.0 (zstd).

Both libraries are present transitively in our vcpkg build (zlib via
`harfbuzz`/`curl`/`ktx`; zstd via `curl`/`ffmpeg`). `Engine/CMakeLists.txt`
detects them via `find_package(ZLIB)` / `find_package(zstd CONFIG)`,
sets `GE_FBTBLEND_HAVE_ZLIB` / `GE_FBTBLEND_HAVE_ZSTD` accordingly, and
links what is found. Uncompressed `.blend` files always parse.

## Bumping to a newer commit

1. Identify a new commit on the upstream `master` branch.
2. Replace `fbtBlend.h` and `Blender.h` from the latest version folder.
   Keep the file names the same.
3. Re-apply the changes under "Local modifications" below, or drop the
   ones upstream has fixed on the same paths.
4. Replace `LICENSE` only if upstream changed it (rare; pure Zlib).
5. Update the commit hash + date + message above.
6. Build `Engine` target and run `BlendSmokeParseTests` (Phase B1) to
   confirm the bump still parses our 3 sample bundles and still rejects
   the hostile inputs cleanly.

## Local modifications

`Blender.h` forward-declares the unused `raw_data` instead of defining it as an empty struct (an empty struct differs in size between C and C++).

`fbtBlend.h` has these strict-build changes:

- `fbtCharHashKey`, `fbtSizeHashKey` and `fbtFixedString` explicitly default
  their copy assignment operators alongside their existing copy constructors.
- `fbtFile::reflect` uses `snprintf` with the version buffer size instead of
  the unbounded `sprintf`.

`fbtMemoryStream` also differs from the pinned upstream commit:

- Every release of `m_buffer` outside a reallocation goes through
  `fbtMemoryStream::release()`, which `delete[]`s it, nulls it and zeroes
  size, capacity and position. Upstream left `m_buffer` dangling after a
  failed zstd decode and the gzip fallback freed it again, so any
  compressed `.blend` that was not valid zstd (a gzip legacy file, a
  truncated file, junk) corrupted the heap (0xC0000374). Upstream also
  `free()`d `new[]` memory when `inflateInit2` failed, and took the
  caller's input as its own buffer on empty zstd input.
- The memory-buffer `open(const void*, FBTsize, StreamMode, bool)`
  releases the previous buffer before loading a new one.
- `gzipInflate` sets `m_size` to the inflated byte count before it grows
  the buffer (upstream's growth copied zero bytes and lost everything
  inflated so far), and an inflate error is a failed decode instead of a
  success with partial output.
- `gzipInflate` grows its buffer geometrically (by half the capacity plus
  half the input) instead of by a fixed half of the input, so inflating
  a highly compressed file copies linearly, not quadratically.
- The copy constructor and copy assignment are deleted: the stream owns
  `m_buffer`, and upstream's implicit copies would free it twice.

The block and type-table readers also differ, so a truncated `.blend`, or
one whose type tables are corrupt, is refused instead of read past its
buffer:

- `fbtChunk::read` refuses a block header the file ends inside, and
  `fbtFile::parseStreamImpl` refuses a block longer than the rest of the
  file before it allocates the block's declared length, and a short read
  after it, freeing the block. Upstream accepted any non-empty read, so a
  short header left the block's fields uninitialised and a short block
  handed its unread, uninitialised tail to the readers.
- `fbtBinTables::read` bounds every read of the SDNA tables by the
  block's length: each table tag and count, every name and type string
  (its terminator must lie inside the block), the type lengths and every
  struct entry. A struct's type, member count, member types and member
  names must index the tables read before it. A name's array dimensions
  stop at the end of the name and at `FBT_ARRAY_SLOTS`, are decimal digits,
  never empty or zero, and multiply to at most 65535 elements (a struct's
  length is 16 bits). Upstream scanned for `]` and for terminators without
  a bound and wrote `m_slots` past its end (a heap overflow on a truncated
  uncompressed file), and a zero dimension divided by zero when a data
  block was linked.

A file whose type tables are in bounds but whose data blocks or struct
layout are corrupt can still fail after the tables are read (a block count
past its length, a block's struct index at the struct count, a struct that
contains itself): issue #4090.

`BlendSmokeParseTests` feeds junk, a truncated zstd blend, a gzip legacy
blend, truncations of a minimal blend (raw and inside a valid gzip or zstd
archive), type tables cut short inside their block, type tables with an
index or an array dimension out of range, and a block longer than its file
through the in-memory parse the model loader uses. Run it under the ASan
preset (`vs2026-x64-local-asan`) as well to see an overread as a failure;
its parse-time budget is relaxed there.

## Why vendored, not vcpkg

fbtBlend is two header files plus a 50-byte `LICENSE` and is not on
vcpkg. Vendoring (a) pins a commit hash for DNA-version
reproducibility, (b) follows existing engine practice for
`stb_image`, `tinygltf`, `ufbx`, and (c) avoids a vcpkg port
maintenance burden for a library that is still effectively a single
maintainer's project.
