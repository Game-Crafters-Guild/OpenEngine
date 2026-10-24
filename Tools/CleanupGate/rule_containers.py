"""Class 03, containers and ids: node-based standard containers, string keys and local hash functions.

The owners of FlatMap, Fnv1a, StringId and the handle container (Types, GenerationalVector) are out of scope. std::function is
counted by the inventory but not gated: whether it costs anything depends on the path it sits on.
"""
import re

from gate_common import Site, strip_strings

CLASS_ID = "03_containers"

kKinds = ["map", "unordered_map", "set", "unordered_set", "list", "multimap", "unordered_multimap", "multiset", "unordered_multiset"]
kContainer = re.compile(r"\b(?:std::)?(" + "|".join(kKinds) + r"|HashMap|HashSet)\s*<\s*([^,>]+)")
kAlias = {"HashMap": "unordered_map", "HashSet": "unordered_set"}
kMapKinds = {"map", "unordered_map", "multimap", "unordered_multimap"}
kSetKinds = {"set", "unordered_set", "multiset", "unordered_multiset"}
kLocalHash = re.compile(r"struct\s+(\w*Hash\w*)\b|\bhash_combine\s*\(|\bstd::hash\s*<|0x9e3779b9|\^=\s*\w+\s*\+\s*0x|\bFnv\w*\s*\(|\bfnv")
kStringKey = re.compile(r"^(?:const\s+)?std::(string|string_view)\b|^(?:const\s+)?(?:char\s*\*|const char\*)")
kIdTypeHint = re.compile(r"guid|asset|entity|component|material|texture|mesh|shader|pass|resource|handle|id\b|name", re.IGNORECASE)
PATTERNS = [prefix + kind for prefix in ("std_", "string_key_") for kind in kKinds] + ["local_hash"]
kExcludedModules = {"Types", "GenerationalVector"}


def _piece_for(kind, is_string_key, raw):
    if kind in kMapKinds:
        piece = "FlatMap (Types/FlatMap.h)"
    elif kind in kSetKinds:
        piece = "FlatSet (Types/FlatSet.h)"
    else:
        piece = "std::vector, or GenerationalVector for handles"
    if is_string_key:
        piece += "; key: StringId or the engine id type" if kIdTypeHint.search(raw) else "; key: is there an id instead of a string?"
    return piece


class Rule:
    def __init__(self, files):
        pass

    def scan_file(self, source):
        if source.module in kExcludedModules:
            return []
        sites = []
        for line_number, code, raw in source.code:
            if not code.strip():
                continue
            code = strip_strings(code)
            for match in kContainer.finditer(code):
                kind, key = match.group(1), match.group(2).strip()
                if kind not in kAlias and not match.group(0).startswith("std::"):
                    continue
                kind = kAlias.get(kind, kind)
                is_string_key = bool(kStringKey.match(key))
                pattern = ("string_key_" if is_string_key else "std_") + kind
                sites.append(Site(source.path, line_number, pattern, _piece_for(kind, is_string_key, raw)))
            for match in kLocalHash.finditer(code):
                if "Fnv" in match.group(0) or "fnv" in match.group(0):
                    continue
                sites.append(Site(source.path, line_number, "local_hash", "Fnv1a64, Fnv1a64Value or HashCombine (Types/Fnv1a.h); a std::hash specialization only for an engine id type"))
        return sites

    def finish(self):
        return []
