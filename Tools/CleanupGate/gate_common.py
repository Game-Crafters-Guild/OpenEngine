"""Shared pieces of the cleanup gate: the production scope, the module a path belongs to, comment stripping and the site record."""
import io
import os
import re
import subprocess
from collections import namedtuple

# One counted occurrence of a pattern: the file relative to the source root, the 1-based line (0 for a finding that spans files),
# the pattern name and the engine piece to use instead.
Site = namedtuple("Site", "file line pattern piece")

# One production source file: the path relative to the source root, its module and its lines with comments and raw string
# literals blanked, as (line_number, code, raw_text).
Source = namedtuple("Source", "path module code")

kScopeFolders = re.compile(r"^(Engine/Source/|Engine/Include/|Engine/ECSModules/|Engine/Modules/[^/]+/(Source|Include)/|Apps/Editor/|Apps/Player/)")
kScopeExtensions = (".cpp", ".h", ".hpp", ".mm", ".inl")
kExcludedFolders = re.compile(r"(^|/)(ThirdParty|Tests|External|Examples|Tools)/")
kGeneratedFiles = re.compile(r"\.generated\.|_generated\.|/Generated/")


def scope_files(root):
    """Returns the production C++ sources git tracks, sorted: every module's Source and Include folders, the engine's own sources
    and the applications, without third-party, test, example, tool or generated files. An untracked file is out of scope until it
    is added, so a scratch file can never be written into the baseline."""
    try:
        listing = subprocess.run(["git", "-C", root, "ls-files", "-z", "--", "Engine", "Apps"],
                                 capture_output=True, check=True).stdout.decode("utf-8")
    except (OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"cleanup gate: the scope comes from `git ls-files`, which failed under {root} ({error}); run the gate in a checkout")
    files = []
    for path in listing.split("\0"):
        if not path.endswith(kScopeExtensions) or not kScopeFolders.match(path):
            continue
        if kExcludedFolders.search(path) or kGeneratedFiles.search(path):
            continue
        if os.path.isfile(os.path.join(root, path)):
            files.append(path)
    return sorted(files)


def module_of(path):
    parts = path.split("/")
    if path.startswith("Engine/Modules/"):
        return parts[2]
    if path.startswith("Engine/ECSModules/"):
        return "ECSModules." + parts[2]
    if path.startswith("Engine/Source/") or path.startswith("Engine/Include/"):
        return "Engine." + (parts[2] if len(parts) > 3 else "root")
    if path.startswith("Apps/Editor/Modules/"):
        return "Editor.Modules." + parts[3]
    if path.startswith("Apps/Editor/"):
        return "Editor." + (parts[3] if len(parts) > 4 else "root")
    if path.startswith("Apps/Player/"):
        return "Player"
    return "other"


def read_lines(root, path):
    with io.open(os.path.join(root, path), "r", encoding="utf-8", errors="replace") as file:
        return file.read().split("\n")


# The opening of a raw string literal, R"delimiter( with an optional encoding prefix, not inside an identifier.
kRawStringOpening = re.compile(r'(?<![A-Za-z0-9_])(?:u8|u|U|L)?R"([^()\\\s"]{0,16})\(')


def code_lines(lines):
    """Yields (line_number, code_text, raw_text) with // and /* */ comments blanked, each raw string literal replaced by "" (also
    across lines, so the text inside one is never read as code) and other string literals kept."""
    in_block_comment = False
    raw_string_end = None
    for line_number, raw in enumerate(lines, 1):
        out = []
        index = 0
        while index < len(raw):
            if raw_string_end is not None:
                end = raw.find(raw_string_end, index)
                if end < 0:
                    break
                index = end + len(raw_string_end)
                raw_string_end = None
                continue
            if in_block_comment:
                end = raw.find("*/", index)
                if end < 0:
                    break
                in_block_comment = False
                index = end + 2
                continue
            if raw.startswith("//", index):
                break
            if raw.startswith("/*", index):
                in_block_comment = True
                index += 2
                continue
            opening = kRawStringOpening.match(raw, index) if raw[index] in "uULR" else None
            if opening:
                out.append('""')
                raw_string_end = ")" + opening.group(1) + '"'
                index = opening.end()
                continue
            out.append(raw[index])
            index += 1
        yield line_number, "".join(out), raw


def load_source(root, path):
    return Source(path, module_of(path), list(code_lines(read_lines(root, path))))


kStringLiteral = re.compile(r'"(?:\\.|[^"\\])*"')


def strip_strings(code):
    """Blanks string literals so a keyword inside a message is not a site."""
    return kStringLiteral.sub('""', code)


def literal_end(raw, index):
    """Returns the index after the string or character literal that opens at raw[index], or the line's end when it does not close."""
    quote = raw[index]
    index += 1
    while index < len(raw):
        if raw[index] == "\\":
            index += 2
            continue
        if raw[index] == quote:
            return index + 1
        index += 1
    return len(raw)


def is_digit_separator(raw, index):
    """True when the ' at raw[index] separates digits, as in 1'000, and does not open a character literal, as in L'x'."""
    before = raw[index - 1] if index > 0 else ""
    after = raw[index + 1] if index + 1 < len(raw) else ""
    closing = raw[index + 2] if index + 2 < len(raw) else ""
    return before.isalnum() and after.isalnum() and closing != "'"


def comment_lines(lines):
    """Yields (line_number, comment_text, code_text): the text of the line's // and /* */ comments joined, and the rest of the line.
    comment_text is None on a line with no comment, and "" on an empty line inside a block comment. A comment marker inside a
    string, character or raw string literal is code."""
    in_block_comment = False
    raw_string_end = None
    for line_number, raw in enumerate(lines, 1):
        comment = []
        code = []
        has_comment = in_block_comment
        index = 0
        while index < len(raw):
            if raw_string_end is not None:
                end = raw.find(raw_string_end, index)
                stop = len(raw) if end < 0 else end + len(raw_string_end)
                code.append(raw[index:stop])
                raw_string_end = raw_string_end if end < 0 else None
                index = stop
                continue
            if in_block_comment:
                end = raw.find("*/", index)
                comment.append(raw[index:] if end < 0 else raw[index:end])
                in_block_comment = end < 0
                index = len(raw) if end < 0 else end + 2
                continue
            if raw.startswith("//", index):
                comment.append(raw[index + 2:])
                has_comment = True
                break
            if raw.startswith("/*", index):
                in_block_comment = True
                has_comment = True
                index += 2
                continue
            opening = kRawStringOpening.match(raw, index) if raw[index] in "uULR" else None
            if opening:
                code.append(raw[index:opening.end()])
                raw_string_end = ")" + opening.group(1) + '"'
                index = opening.end()
                continue
            if raw[index] == '"' or (raw[index] == "'" and not is_digit_separator(raw, index)):
                end = literal_end(raw, index)
                code.append(raw[index:end])
                index = end
                continue
            code.append(raw[index])
            index += 1
        yield line_number, " ".join(comment) if has_comment else None, "".join(code)
