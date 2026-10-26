"""Class 02, strings, numbers and paths: hand-written split, trim, join, case folds, prefix and suffix tests, number formatting and
parsing, path joining.

The owner of the string utilities (Types) is out of scope. std::to_string (integers are locale-free and a regex cannot tell them
from floats) and std::format (locale-free) are counted by the inventory but not gated.
"""
import re

from gate_common import Site, strip_strings

CLASS_ID = "02_strings"

kPatterns = [
    ("split_getline", re.compile(r"std::getline\s*\("), "Split (Types/StringUtils.h)"),
    ("split_stringstream", re.compile(r"std::i?stringstream\b"), "Split (Types/StringUtils.h), or Types/ParseNumber.h for a number"),
    ("split_find_substr", re.compile(r"\.find(?:_first_of)?\s*\([^)]*\)[^;]*\.substr\s*\(|\.substr\s*\([^;]*\.find(?:_first_of)?\s*\("), "Split (Types/StringUtils.h)"),
    ("trim_find_not_of", re.compile(r"find_(?:first|last)_not_of\s*\(\s*\"[ \\t\\r\\n]"), "TrimWhitespace or TrimWhitespaceView (Types/StringUtils.h)"),
    ("trim_isspace_loop", re.compile(r"\bisspace\s*\("), "TrimWhitespace or TrimWhitespaceView (Types/StringUtils.h)"),
    ("case_tolower", re.compile(r"\b(?:std::)?tolower\s*\(|\b(?:std::)?toupper\s*\(|std::transform\s*\([^;]*(?:tolower|toupper)"), "ToLowerAscii or ToUpperAscii (Types/StringUtils.h)"),
    ("case_stricmp", re.compile(r"\b(?:_stricmp|_strnicmp|strcasecmp|strncasecmp|_wcsicmp|CompareStringOrdinal)\s*\("), "EqualsIgnoreCase or StartsWithIgnoreCase (Types/StringUtils.h)"),
    ("startswith_rfind0", re.compile(r"\.rfind\s*\([^;]*,\s*0\s*\)\s*==\s*0"), "std::string_view::starts_with, or StartsWithIgnoreCase (Types/StringUtils.h)"),
    ("startswith_compare0", re.compile(r"\.compare\s*\(\s*0\s*,"), "std::string_view::starts_with"),
    ("endswith_compare", re.compile(r"\.compare\s*\([^;]*\.(?:size|length)\s*\(\)\s*-"), "std::string_view::ends_with, or EndsWithIgnoreCase (Types/StringUtils.h)"),
    ("startswith_substr0", re.compile(r"\.substr\s*\(\s*0\s*,[^;]*\)\s*==\s*\""), "std::string_view::starts_with"),
    ("strncmp_prefix", re.compile(r"\bstrncmp\s*\("), "std::string_view::starts_with"),
    ("format_snprintf", re.compile(r"\b(?:std::)?(?:snprintf|sprintf|_snprintf|vsnprintf)\s*\("), "FormatFloat or FormatFixed (Types/FormatNumber.h), or the Logger's formatting"),
    ("format_ostringstream", re.compile(r"std::ostringstream\b"), "FormatFloat or FormatFixed (Types/FormatNumber.h), or std::format"),
    ("format_setprecision", re.compile(r"std::setprecision|std::fixed\b"), "FormatFixed (Types/FormatNumber.h)"),
    ("parse_stox", re.compile(r"\bstd::sto(?:f|d|i|l|ul|ll|ull)\s*\("), "ParseInt, ParseUnsigned, ParseFloat or ParseDouble (Types/ParseNumber.h)"),
    ("parse_atox", re.compile(r"\b(?:std::)?(?:atof|atoi|atol|strtof|strtod|strtol|strtoul|strtoll|strtoull)\s*\("), "ParseInt, ParseUnsigned, ParseFloat or ParseDouble (Types/ParseNumber.h)"),
    ("parse_from_chars", re.compile(r"std::from_chars\s*\("), "Types/ParseNumber.h"),
    ("parse_sscanf", re.compile(r"\bsscanf\s*\("), "Types/ParseNumber.h"),
    ("path_join_literal", re.compile(r"""(?:\+\s*["']/["']|["']/["']\s*\+|\+=\s*["']/["']|\+\s*"\\\\"|\+=\s*"\\\\")"""), "std::filesystem::path operator/"),
    ("path_sep_literal_backslash", re.compile(r"""'\\\\'|"\\\\"\s*\+|\+\s*"\\\\\""""), "std::filesystem::path::preferred_separator or generic_string"),
    ("join_sep_append", re.compile(r"""\+=\s*(?:", "|",\s?"|"; "|" "|'\s'|','|';')"""), "Join (Types/StringUtils.h)"),
    ("replace_all_loop", re.compile(r"\.replace\s*\(\s*\w+\s*,\s*\w+(?:\.(?:size|length)\(\))?\s*,"), "ReplaceAll (Types/StringUtils.h)"),
    ("strlen_strcpy", re.compile(r"\b(?:strcpy|strncpy|strcpy_s|strcat|strncat)\s*\("), "std::string, or FixedStringView (Types/StringUtils.h) for a fixed buffer"),
]
PATTERNS = [name for name, _, _ in kPatterns]
# The patterns that read a string literal's text (a separator, a whitespace set); every other pattern reads the line with its
# literals blanked, so a keyword inside a message is not a site.
kLiteralPatterns = {"trim_find_not_of", "path_join_literal", "path_sep_literal_backslash", "join_sep_append"}
kExcludedModules = {"Types"}


class Rule:
    def __init__(self, files):
        pass

    def scan_file(self, source):
        if source.module in kExcludedModules:
            return []
        sites = []
        for line_number, code, _ in source.code:
            if not code.strip():
                continue
            blanked = strip_strings(code)
            for name, pattern, piece in kPatterns:
                if pattern.search(code if name in kLiteralPatterns else blanked):
                    sites.append(Site(source.path, line_number, name, piece))
        return sites

    def finish(self):
        return []
