#!/usr/bin/env python3
"""Convert a project's scene blueprints to prefabs.

Usage:
    python migrate_blueprints_to_prefabs.py [--dry-run] <project folder>

Under the folder, the script:
  - renames every `Name.blueprint` to `Name.prefab`;
  - in every `.scene`, `.prefab` and `.blueprint` file, rewrites the `[blueprint version=1]` header
    and each `[blueprint id=... source=...]` instance section to `[prefab ...]`, the `.blueprint`
    of each `path=` on a section header (quoted or not) and of each `[path=... guid=...]` value to
    `.prefab`, and a component name on a line of its own in an instance section to `+Component`;
  - in `AssetDatabase.assetdb`, gives each entry that names a `.blueprint` file the `.prefab` path
    and the type `Prefab`, so every prefab keeps its GUID.

Every other byte is kept: line endings, comments, order, quoting and spacing. Each file it changes
is printed. Files are rewritten in place before a rename, so each keeps its file id. Other files
that mention `.blueprint` (the project's own scripts and documents) are listed and never edited.

The script refuses a folder under version control that has uncommitted changes, so the conversion
is one change a user reviews and can revert. `--dry-run` prints the same list and writes nothing.
Running it twice is a no-op. Close the editor on the project first: an open editor writes
`AssetDatabase.assetdb` and can write the old entries back.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

OLD_EXTENSION = ".blueprint"
NEW_EXTENSION = ".prefab"
SCENE_TEXT_EXTENSIONS = {".scene", ".prefab", OLD_EXTENSION}
ASSET_DATABASE_NAME = "AssetDatabase.assetdb"
PROJECT_CODE_EXTENSIONS = {".py", ".cs", ".cpp", ".h", ".hpp", ".json", ".md", ".mdx", ".txt", ".cmake", ".sh", ".ps1"}
UTF8_BOM = b"\xef\xbb\xbf"
SKIPPED_FOLDERS = {".git", "build", "node_modules", ".Cache", "Cache", "Intermediate", "Saved"}

# `path="x.blueprint"` or `path=x.blueprint`, on a header or inside a `[path=... guid=...]` value.
PATH_ATTRIBUTE = re.compile(rb'(?i)(\bpath\s*=\s*)("?)([^"\]\s]*?)\.blueprint(\2)')
ANY_OLD_EXTENSION = re.compile(rb"(?i)\.blueprint\b")
ASSET_DATABASE_PATH = re.compile(rb'("path"\s*:\s*")([^"]*?)\.blueprint(")', re.IGNORECASE)
ASSET_DATABASE_TYPE = re.compile(rb'("type"\s*:\s*")[^"]*(")')


def section_type(stripped: bytes) -> str | None:
    """The lowercased type of a section header line (`[type key=value ...]`), or None."""
    if len(stripped) < 2 or stripped[:1] != b"[" or stripped[-1:] != b"]":
        return None
    body = stripped[1:-1].strip()
    end = 0
    while end < len(body) and not body[end:end + 1].isspace():
        end += 1
    return body[:end].decode("ascii", "replace").lower() or None


def with_section_type(line: bytes, new_type: bytes) -> bytes:
    """`line` with its section type token replaced and every other byte kept."""
    lead = len(line) - len(line.lstrip())
    rest = line[lead:]
    end = 1
    while end < len(rest) and not rest[end:end + 1].isspace() and rest[end:end + 1] != b"]":
        end += 1
    return line[:lead] + b"[" + new_type + rest[end:]


def with_prefab_paths(line: bytes) -> bytes:
    return PATH_ATTRIBUTE.sub(lambda m: m.group(1) + m.group(2) + m.group(3) + NEW_EXTENSION.encode() + m.group(4), line)


def convert_scene_text(data: bytes) -> bytes:
    """The prefab form of one scene, prefab or blueprint file."""
    if data.startswith(UTF8_BOM):
        return UTF8_BOM + convert_scene_text(data[len(UTF8_BOM):])
    out: list[bytes] = []
    first_section = True
    in_instance = False
    for line in data.splitlines(keepends=True):
        stripped = line.strip()
        kind = section_type(stripped)
        if kind is not None:
            in_instance = kind in ("blueprint", "prefab") and not first_section
            if kind == "blueprint":
                line = with_section_type(line, b"prefab")
            out.append(with_prefab_paths(line))
            first_section = False
            continue
        if in_instance and stripped and stripped[:1] not in (b";", b"+", b"-") and b"=" not in stripped \
                and b"." not in stripped:
            lead = len(line) - len(line.lstrip())
            line = line[:lead] + b"+" + line[lead:]
        elif b"=" in stripped and ANY_OLD_EXTENSION.search(stripped):
            line = with_prefab_paths(line)
        out.append(line)
    return b"".join(out)


def convert_asset_database(data: bytes) -> bytes:
    out: list[bytes] = []
    for line in data.splitlines(keepends=True):
        if ASSET_DATABASE_PATH.search(line):
            line = ASSET_DATABASE_PATH.sub(lambda m: m.group(1) + m.group(2) + NEW_EXTENSION.encode() + m.group(3), line)
            line = ASSET_DATABASE_TYPE.sub(lambda m: m.group(1) + b"Prefab" + m.group(2), line, count=1)
        out.append(line)
    return b"".join(out)


def project_files(root: Path):
    for folder, subfolders, names in os.walk(root):
        subfolders[:] = sorted(name for name in subfolders if name not in SKIPPED_FOLDERS)
        for name in sorted(names):
            yield Path(folder) / name


def uncommitted_changes(root: Path) -> list[str] | None:
    """The uncommitted paths of the work tree holding `root`, or None when no git work tree does."""
    try:
        inside = subprocess.run(["git", "-C", str(root), "rev-parse", "--is-inside-work-tree"],
                                capture_output=True, text=True)
    except OSError:
        return None
    if inside.returncode != 0 or inside.stdout.strip() != "true":
        return None
    status = subprocess.run(["git", "-C", str(root), "status", "--porcelain", "--", "."],
                            capture_output=True, text=True, check=True)
    return [line for line in status.stdout.splitlines() if line.strip()]


def rewrite_in_place(path: Path, data: bytes) -> None:
    # Truncating and writing the same file keeps its file id, which the asset database matches a
    # renamed file by.
    with open(path, "r+b") as f:
        f.truncate()
        f.write(data)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("project", help="the project folder")
    parser.add_argument("--dry-run", action="store_true", help="print what would change; write nothing")
    args = parser.parse_args(argv)

    root = Path(args.project).resolve()
    if not root.is_dir():
        print(f"error: {root} is not a folder", file=sys.stderr)
        return 2
    changes = uncommitted_changes(root)
    if changes:
        print(f"error: {root} has {len(changes)} uncommitted change(s). Commit or stash them, then run the "
              "script again, so the conversion is one change you can review and revert.", file=sys.stderr)
        return 2
    if changes is None:
        print(f"note: {root} is not in a git work tree; the conversion cannot be reverted from version control.")

    verb = "would change" if args.dry_run else "changed"
    changed = 0
    renames: list[tuple[Path, Path]] = []
    mentions: list[str] = []
    for path in project_files(root):
        relative = path.relative_to(root).as_posix()
        extension = path.suffix.lower()
        if extension in SCENE_TEXT_EXTENSIONS or path.name == ASSET_DATABASE_NAME:
            data = path.read_bytes()
            converted = convert_asset_database(data) if path.name == ASSET_DATABASE_NAME else convert_scene_text(data)
            if converted != data:
                changed += 1
                print(f"{verb}: {relative}")
                if not args.dry_run:
                    rewrite_in_place(path, converted)
            if extension == OLD_EXTENSION:
                renames.append((path, path.with_suffix(NEW_EXTENSION)))
            for number, line in enumerate(converted.splitlines(), start=1):
                if ANY_OLD_EXTENSION.search(line) or re.search(rb"\[blueprint\b", line):
                    mentions.append(f"{relative}:{number}")
        elif extension in PROJECT_CODE_EXTENSIONS:
            try:
                if ANY_OLD_EXTENSION.search(path.read_bytes()):
                    mentions.append(relative)
            except OSError:
                continue

    failed = False
    for source, target in renames:
        source_relative = source.relative_to(root).as_posix()
        target_relative = target.relative_to(root).as_posix()
        if target.exists():
            print(f"error: {target_relative} already exists; {source_relative} is not renamed", file=sys.stderr)
            failed = True
            continue
        print(f"{'would rename' if args.dry_run else 'renamed'}: {source_relative} -> {target_relative}")
        if not args.dry_run:
            os.rename(source, target)

    print(f"{len(renames)} prefab(s) {'to rename' if args.dry_run else 'renamed'}, {changed} file(s) {verb}.")
    if mentions:
        print("These still mention .blueprint and are not edited (comments, text values, the project's own code):")
        for mention in mentions:
            print(f"  {mention}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
