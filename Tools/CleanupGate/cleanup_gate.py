"""The cleanup gate: the patterns the coding style replaces with an engine piece may only become fewer.

Scans the production sources (gate_common.scope_files: the files git tracks) with the rules of the ten classes (rule_*.py). A
rule's REPORTED patterns are counted and printed, never compared; a rule's inventory_lines(sites), when it has one, prints the
counted sites grouped for a sweep to pick from. The sites of a boundary that must stay raw are counted apart, against
allowlist.txt: one `path:pattern:count  # boundary` per file and pattern. Every other site is counted against baseline.json (class,
pattern, file, count).

The gate fails when:
- a pattern has more sites than its baseline: each file above its own baseline is named with every site of the pattern in it and
  the engine piece to use;
- a pattern has fewer sites than its baseline: the baseline is lowered in the same change, with --update-baseline;
- a file has more sites of an allowlisted pattern than its entry allows: the sites are named;
- a file has fewer sites of an allowlisted pattern than its entry, or none: the entry is lowered or deleted, with --update-baseline;
- an allowlist line is malformed, listed twice or names a pattern no rule has.

--update-baseline writes the current counts to baseline.json and lowers or deletes the allowlist entries that fell; it refuses
when any count rose, in the baseline or in the allowlist.

Run: python -I Tools/CleanupGate/cleanup_gate.py [--root <source root>] [--baseline <file>] [--allowlist <file>] [--update-baseline]
"""
import argparse
import json
import os
import sys
import time
from collections import defaultdict, namedtuple
from concurrent.futures import ProcessPoolExecutor

sys.dont_write_bytecode = True
kGateFolder = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, kGateFolder)

import gate_common  # noqa: E402
import rule_comments  # noqa: E402
import rule_containers  # noqa: E402
import rule_files_organization  # noqa: E402
import rule_log_memory_time  # noqa: E402
import rule_math_helpers  # noqa: E402
import rule_math_types  # noqa: E402
import rule_rendering  # noqa: E402
import rule_strings  # noqa: E402
import rule_threads  # noqa: E402
import rule_ui_controls  # noqa: E402

kRuleModules = [rule_math_types, rule_strings, rule_containers, rule_files_organization, rule_ui_controls, rule_threads,
                rule_log_memory_time, rule_rendering, rule_math_helpers, rule_comments]
kUpdateCommand = "python -I Tools/CleanupGate/cleanup_gate.py --update-baseline"

# One allowlist line: the entry's file, pattern and allowed count with the boundary that keeps it raw; or, for a comment or a
# blank line, file None and the line's text in `boundary`.
AllowlistLine = namedtuple("AllowlistLine", "file pattern count boundary")


def pattern_classes():
    """Returns {pattern: class id}; a pattern name belongs to one class."""
    owners = {}
    for module in kRuleModules:
        for pattern in module.PATTERNS:
            if pattern in owners:
                raise SystemExit(f"cleanup gate: pattern {pattern} is defined by {owners[pattern]} and {module.CLASS_ID}")
            owners[pattern] = module.CLASS_ID
    return owners


def scan_class(root, files, index):
    """Returns the sites of one class over every file; runs in a worker process, one per class."""
    rule = kRuleModules[index].Rule(files)
    sites = []
    for path in files:
        sites.extend(rule.scan_file(gate_common.load_source(root, path)))
    sites.extend(rule.finish())
    return sites


def scan(root):
    files = gate_common.scope_files(root)
    classes = len(kRuleModules)
    with ProcessPoolExecutor(max_workers=classes) as pool:
        per_class = pool.map(scan_class, [root] * classes, [files] * classes, range(classes))
        sites = [site for class_sites in per_class for site in class_sites]
    return files, sites


def read_allowlist(path, owners):
    """Returns the allowlist's lines in order; fails on a malformed line, a duplicate or a pattern no rule has."""
    lines = []
    seen = set()
    with open(path, "r", encoding="utf-8") as file:
        for number, text in enumerate(file, 1):
            entry, _, boundary = text.partition("#")
            entry = entry.strip()
            if not entry:
                lines.append(AllowlistLine(None, None, 0, text.rstrip("\n")))
                continue
            parts = entry.rsplit(":", 2)
            if len(parts) != 3 or not parts[0] or not parts[2].isdigit() or not boundary.strip():
                raise SystemExit(f"cleanup gate: {path}:{number}: expected `path:pattern:count  # boundary`, found `{text.strip()}`")
            file_path, pattern, allowed = parts
            if pattern not in owners:
                raise SystemExit(f"cleanup gate: {path}:{number}: no rule has the pattern `{pattern}`")
            if (file_path, pattern) in seen:
                raise SystemExit(f"cleanup gate: {path}:{number}: `{file_path}:{pattern}` is listed twice")
            seen.add((file_path, pattern))
            lines.append(AllowlistLine(file_path, pattern, int(allowed), boundary.strip()))
    return lines


def write_allowlist(path, lines, allowlisted):
    """Writes the allowlist with each entry's count lowered to its sites today; an entry with no site left is dropped."""
    entries = [line for line in lines if line.file is not None and allowlisted.get((line.file, line.pattern))]
    width = max((len(f"{line.file}:{line.pattern}:{allowlisted[(line.file, line.pattern)]}") for line in entries), default=0)
    with open(path, "w", encoding="utf-8", newline="\n") as file:
        for line in lines:
            if line.file is None:
                file.write(line.boundary + "\n")
                continue
            sites = allowlisted.get((line.file, line.pattern))
            if sites:
                file.write(f"{line.file}:{line.pattern}:{sites}".ljust(width) + f"  # {line.boundary}\n")


def count(sites, owners):
    """Returns {class: {pattern: {file: count}}} with every pattern of every class present."""
    counts = {}
    for pattern, class_id in owners.items():
        counts.setdefault(class_id, {})[pattern] = {}
    for site in sites:
        per_file = counts[owners[site.pattern]][site.pattern]
        per_file[site.file] = per_file.get(site.file, 0) + 1
    return counts


def total(per_file):
    return sum(per_file.values())


def find_rises(counts, baseline, sites_by_key):
    """Returns the failure lines for every pattern whose count rose, naming the files above their own baseline."""
    lines = []
    for class_id, patterns in counts.items():
        for pattern, per_file in patterns.items():
            base = baseline.get(class_id, {}).get(pattern, {})
            if total(per_file) <= total(base):
                continue
            lines.append(f"FAIL {class_id} {pattern}: {total(per_file)} sites, baseline {total(base)}. The sites in the files above their baseline:")
            for path in sorted(per_file):
                if per_file[path] <= base.get(path, 0):
                    continue
                for site in sites_by_key[(path, pattern)]:
                    lines.append(f"  {path}:{site.line} ({per_file[path]} in the file, baseline {base.get(path, 0)}): use {site.piece}")
    return lines


def find_drops(counts, baseline):
    lines = []
    for class_id, patterns in counts.items():
        for pattern, per_file in patterns.items():
            base = baseline.get(class_id, {}).get(pattern, {})
            if total(per_file) < total(base):
                lines.append(f"FAIL {class_id} {pattern}: {total(per_file)} sites, baseline {total(base)}: lower the baseline in this change with `{kUpdateCommand}`")
    for class_id, patterns in baseline.items():
        for pattern in patterns:
            if pattern not in counts.get(class_id, {}):
                lines.append(f"FAIL {class_id} {pattern}: the baseline names a pattern no rule has: rewrite it with `{kUpdateCommand}`")
    return lines


def compare_allowlist(lines, allowlisted, sites_by_key):
    """Returns (rises, drops): the failure lines for the entries whose file has more sites than allowed, and for those with fewer."""
    rises, drops = [], []
    for line in lines:
        if line.file is None:
            continue
        sites = allowlisted.get((line.file, line.pattern), 0)
        if sites > line.count:
            rises.append(f"FAIL allowlist {line.file}:{line.pattern}: {sites} sites, the entry allows {line.count} ({line.boundary}). The sites:")
            for site in sites_by_key[(line.file, line.pattern)]:
                rises.append(f"  {line.file}:{site.line}: use {site.piece}, or raise the entry if this site is the same boundary")
        elif sites == 0:
            drops.append(f"FAIL allowlist {line.file}:{line.pattern} matches no site: delete the entry, or run `{kUpdateCommand}`")
        elif sites < line.count:
            drops.append(f"FAIL allowlist {line.file}:{line.pattern}: {sites} sites, the entry allows {line.count}: lower it with `{kUpdateCommand}`")
    return rises, drops


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=os.path.dirname(os.path.dirname(kGateFolder)), help="the source root (default: the tree holding this script)")
    parser.add_argument("--baseline", default=os.path.join(kGateFolder, "baseline.json"))
    parser.add_argument("--allowlist", default=os.path.join(kGateFolder, "allowlist.txt"))
    parser.add_argument("--update-baseline", action="store_true", help="write the current counts; refused when a count rose")
    arguments = parser.parse_args()

    start = time.monotonic()
    owners = pattern_classes()
    allowlist_lines = read_allowlist(arguments.allowlist, owners)
    allowlist_keys = {(line.file, line.pattern) for line in allowlist_lines if line.file is not None}
    files, sites = scan(arguments.root)
    reported_patterns = {pattern for module in kRuleModules for pattern in getattr(module, "REPORTED", [])}
    reported = defaultdict(int)
    for site in sites:
        if site.pattern in reported_patterns:
            reported[site.pattern] += 1
    sites = [site for site in sites if site.pattern not in reported_patterns]
    sites_by_key = defaultdict(list)
    for site in sites:
        sites_by_key[(site.file, site.pattern)].append(site)
    allowlisted = {key: len(sites_by_key[key]) for key in allowlist_keys}
    counted = [site for site in sites if (site.file, site.pattern) not in allowlist_keys]
    counts = count(counted, owners)
    with open(arguments.baseline, "r", encoding="utf-8") as file:
        baseline = json.load(file)

    print(f"cleanup gate: {len(files)} files under {arguments.root}; {sum(allowlisted.values())} allowlisted sites in {len(allowlist_keys)} entries; {time.monotonic() - start:.1f} s")
    for class_id, patterns in counts.items():
        current = sum(total(per_file) for per_file in patterns.values())
        base = sum(total(per_file) for per_file in baseline.get(class_id, {}).values())
        print(f"{class_id}: {current} sites, baseline {base}")
    for pattern in sorted(reported_patterns):
        print(f"reported, not gated: {pattern} {reported[pattern]}")
    for module in kRuleModules:
        if hasattr(module, "inventory_lines"):
            for line in module.inventory_lines(counted):
                print(f"inventory: {line}")

    rises = find_rises(counts, baseline, sites_by_key)
    allowlist_rises, allowlist_drops = compare_allowlist(allowlist_lines, allowlisted, sites_by_key)
    if arguments.update_baseline:
        if rises or allowlist_rises:
            print("\n".join(rises + allowlist_rises))
            print("cleanup gate: nothing is raised; move the new sites to the engine piece, or allowlist a boundary that must stay raw")
            return 1
        with open(arguments.baseline, "w", encoding="utf-8", newline="\n") as file:
            json.dump(counts, file, indent=1, sort_keys=True)
            file.write("\n")
        write_allowlist(arguments.allowlist, allowlist_lines, allowlisted)
        print(f"cleanup gate: wrote {arguments.baseline} and {arguments.allowlist}")
        return 0

    failures = rises + allowlist_rises + find_drops(counts, baseline) + allowlist_drops
    if failures:
        print("\n".join(failures))
        return 1
    print("cleanup gate: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
