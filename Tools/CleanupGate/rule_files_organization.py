"""Class 04, files and organization: one-function files, files named for a function, free-function headers beside a hub, and helpers
copied into another file with the same name and body.

The counts are a structural approximation: a class is a struct or class definition line, a free function is a function head at
namespace scope. Helper names shared by several files are reported, not gated: a shared name is often a different function. Tiny
struct files, files over 1500 lines and headers with many classes are listed by the inventory but not gated.
"""
import os
import re
from collections import defaultdict

from gate_common import Site

CLASS_ID = "04_files_organization"
PATTERNS = ["one_free_function_file", "named_for_a_function", "free_functions_beside_hub", "duplicate_helper_bodies"]
# Counted and printed, never a failure.
REPORTED = ["shared_helper_names"]

kClassDefinition = re.compile(r"^\s*(?:template\s*<[^>]*>\s*)?(?:class|struct)\s+(?:alignas\s*\([^)]*\)\s*)?(?:\w+\s+)?(\w+)\s*(?::[^;{]*)?\{?\s*$|^\s*(?:class|struct)\s+(\w+)\s*(?::[^;{]*)?\{")
kEnumDefinition = re.compile(r"^\s*enum\s+(?:class\s+)?(\w+)")
kFunctionHead = re.compile(r"^\s*(?:\[\[nodiscard\]\]\s*)?(?:template\s*<[^>]*>\s*)?(?:static\s+|inline\s+|constexpr\s+|extern\s+|virtual\s+|explicit\s+)*(?:[\w:<>,\s\*&]+?)\s+([\w:~]+)\s*\(([^;{]*)\)\s*(?:const)?\s*(?:noexcept(?:\([^)]*\))?)?\s*(?:override)?\s*(?:->\s*[\w:<>]+)?\s*(\{|;)?\s*$")
kControlWords = {"if", "for", "while", "switch", "return", "else", "do", "catch", "sizeof", "static_assert", "alignas", "decltype", "define", "namespace", "using", "typedef", "delete", "new", "throw", "co_return"}
kVerbName = re.compile(r"^(Compute|Build|Resolve|Apply|Collect|Make|Create|Get|Set|Find|Load|Save|Parse|Format|Write|Read|Convert|Draw|Emit|Pick|Place|Snap|Rename|Replace|Restore|Register|Scan|Sync|Update|Validate|Derive|Project|Extract|Merge|Split|Fit|Clamp|Pack|Unpack|Encode|Decode|Hash|Measure|Refresh|Run|Launch|Open|Close|Query|Sample|Seed|Spawn|Tick|Step|Walk|Trim|Strip|Ensure|Attach|Detach|Mount|Patch|Record|Capture|Tokenize|Classify|Quantize|Layout|Describe|Prepare|Install|Bind|Resize|Flush|Reset|Clear|Begin|End|Insert|Remove|Append|Try|Is|Has|Can|Should|Select|Choose|Compare|Count|Copy|Move|Fill|Generate|Dump|Print|Report|Warm|Prewarm|Schedule|Dispatch|Submit|Upload|Download|Fetch|Serialize|Deserialize|Export|Import|Compile|Link|Stage|Promote|Adopt|Inherit|Heal|Detect|Surface)[A-Z]")
kOwnerSuffix = re.compile(r"(Service|System|Controller|Manager|Panel|Inspector|Registry|Cache|Builder|Loader|Writer|Reader|Handler|Tracker|Provider|Tests?|Model|View|State|Policy|Queue|Pool|Ring|Context|Collector|Gate|Runner|Surfacer|Launcher|Watchdog|Bridge|Session|Store|Table|Map|Set|Query|Rules?|Spec|Desc|Info|Options|Settings|Plan|Ops)$")
kHubFolders = {"Apps/Editor/Source", "Apps/Editor/Include", "Engine/Include/Engine", "Engine/Source/Engine", "Engine/Include/Core", "Engine/Source/Core", "Engine/Include/Engine/Rendering", "Engine/Source/Engine/Rendering"}
kSourceSuffix = re.compile(r"\.(cpp|h|hpp|mm|inl)$")
# Names every subsystem defines for itself; two files defining one of them is not a duplicate helper.
kCommonNames = {"main", "WinMain", "DllMain", "operator", "Register", "Initialize", "Shutdown", "Update", "Create", "Destroy", "Get", "Set", "Describe", "Tick", "Reset", "Clear", "Apply", "Render", "Draw", "Build", "Load", "Save", "Begin", "End", "Run", "Stop", "Start", "Add", "Remove", "Find", "Compile", "Flush", "Resize", "Poll"}


def _opens_body_on_next_line(code_lines, index):
    """True when the next line with code after code_lines[index] starts with `{`: the Allman form of a function definition."""
    for _, code, _ in code_lines[index + 1:]:
        stripped = code.strip()
        if stripped:
            return stripped.startswith("{")
    return False


def _body_text(code_lines, index):
    """The text of the function body that opens at or after code_lines[index], from its `{` to the matching `}`, with all
    whitespace removed, so two copies that differ only in layout compare equal."""
    parts = []
    depth = 0
    opened = False
    for _, code, _ in code_lines[index:]:
        for character in code:
            if character == "{":
                depth += 1
                opened = True
            if opened and not character.isspace():
                parts.append(character)
            if character == "}" and opened:
                depth -= 1
                if depth == 0:
                    return "".join(parts)
    return "".join(parts)


def _analyze(code_lines):
    """Returns the class definitions, the namespace-scope function heads (name, line, has a body, body text or None), the enum count
    and whether the file defines a member function (Class::Method), which makes a .cpp the implementation of a class rather than a
    loose function."""
    classes, functions, enums = [], [], 0
    defines_member = False
    depth = 0
    class_stack = []
    for index, (line_number, code, _) in enumerate(code_lines):
        stripped = code.strip()
        if not stripped or stripped.startswith("#"):
            continue
        # The substring tests skip the expensive matches on lines that cannot match them.
        class_match = kClassDefinition.match(code) if ("class" in code or "struct" in code) else None
        if class_match and not stripped.endswith(";"):
            classes.append((class_match.group(1) or class_match.group(2), line_number))
            class_stack.append(depth)
        if "enum" in code and kEnumDefinition.match(code) and not stripped.endswith(";"):
            enums += 1
        head = kFunctionHead.match(code) if "(" in code else None
        if head and not class_stack:
            name = head.group(1)
            # An m_ name is a member initializer before an Allman constructor body, never a free function.
            if name.split("::")[-1] not in kControlWords and not name.startswith(("operator", "m_")):
                ending = head.group(3)
                has_body = ending == "{" or (ending is None and _opens_body_on_next_line(code_lines, index))
                if "::" not in name:
                    functions.append((name, line_number, has_body, _body_text(code_lines, index) if has_body else None))
                elif has_body:
                    defines_member = True
        depth += code.count("{") - code.count("}")
        if class_stack and depth <= class_stack[-1]:
            class_stack.pop()
    return classes, functions, enums, defines_member


class Rule:
    def __init__(self, files):
        self.m_HelperFiles = defaultdict(set)
        # (name, body) -> {path: line}: the files that define a helper of that name with that exact body.
        self.m_HelperBodies = defaultdict(dict)

    def scan_file(self, source):
        path = source.path
        classes, functions, enums, defines_member = _analyze(source.code)
        stem = kSourceSuffix.sub("", os.path.basename(path))
        in_hub_folder = os.path.dirname(path) in kHubFolders
        defined = [function for function in functions if function[2]] if path.endswith(".cpp") else functions
        for name, line_number, has_body, body in functions:
            if has_body and path.endswith((".cpp", ".h", ".inl")):
                self.m_HelperFiles[name].add(path)
                self.m_HelperBodies[(name, body)].setdefault(path, line_number)
        sites = []
        if not classes and not defines_member and len(defined) == 1 and enums == 0:
            sites.append(Site(path, defined[0][1], "one_free_function_file", f"the utility header of the module that owns {defined[0][0]}, beside its kin"))
        if kVerbName.match(stem) and not kOwnerSuffix.search(stem):
            sites.append(Site(path, 1, "named_for_a_function", "a file named for the class it holds, or the owner's utility header for the function"))
        if in_hub_folder and not classes and defined and path.endswith(".h"):
            sites.append(Site(path, defined[0][1], "free_functions_beside_hub", "the owning module's utility header for these functions"))
        return sites

    def finish(self):
        sites = []
        # A copy: the same name and the same body, in another file; a renamed copy is not seen. Every file after the first in path
        # order is one site, so a third copy adds a site.
        for (name, _), where in sorted(self.m_HelperBodies.items()):
            if len(where) < 2:
                continue
            paths = sorted(where)
            for path in paths[1:]:
                sites.append(Site(path, where[path], "duplicate_helper_bodies", f"one {name} in the owner's utility header; the same body is in {len(paths)} files ({'; '.join(paths)})"))
        # A shared name is reported, not gated: the same name with a different body is often a different function.
        for name, files in sorted(self.m_HelperFiles.items()):
            if len(files) >= 2 and name not in kCommonNames and len(name) > 3:
                paths = sorted(files)
                sites.append(Site(paths[0], 0, "shared_helper_names", f"{name} is defined in {len(paths)} files ({'; '.join(paths)})"))
        return sites
