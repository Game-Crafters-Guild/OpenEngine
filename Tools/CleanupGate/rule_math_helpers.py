"""Class 09, math helpers: local definitions of lerp, clamp, smoothstep, easing, noise and hash functions, and their inline idioms.

The owners (Mathematics, Noise) are out of scope.
"""
import re

from gate_common import Site, strip_strings

CLASS_ID = "09_math_helpers"
PATTERNS = ["local_definition", "inline_smoothstep", "inline_saturate_ternary", "saturate_std_clamp01", "inline_clamp_minmax", "inline_lerp", "float_hash_constant"]

kFloat = r"(?:float|double|float32|float64|auto|constexpr float|constexpr auto)"
kNames = r"(Lerp|lerp|Mix|mix|Clamp|clamp|Clamp01|Saturate|saturate|Remap|remap|MapRange|InverseLerp|inverseLerp|SmoothStep|smoothstep|Smoothstep|SmootherStep|smootherstep|Step|step|Fract|fract|Frac|Wrap|wrap|Sign|sign|Ease[A-Za-z]*|ease[A-Za-z]*|Hash[A-Za-z0-9]*|hash[A-Za-z0-9]*|Noise[A-Za-z0-9]*|noise[A-Za-z0-9]*|Perlin\w*|perlin\w*|Simplex\w*|simplex\w*|Fbm\w*|fbm\w*|Worley\w*|Voronoi\w*|Radians|Degrees|ToRadians|ToDegrees|DegToRad|RadToDeg|Approximately|NearlyEqual|AlmostEqual|Pow2|NextPow2|IsPow2|RoundUp|AlignUp)"
kLocalDefinition = re.compile(rf"^\s*(?:\[\[nodiscard\]\]\s*)?(?:static\s+|inline\s+|constexpr\s+)*(?:static\s+|inline\s+|constexpr\s+)*{kFloat}\s+{kNames}\s*\([^;]*\)\s*(?:noexcept)?\s*\{{?\s*$")
kLocalDefinitionInteger = re.compile(r"^\s*(?:\[\[nodiscard\]\]\s*)?(?:static\s+|inline\s+|constexpr\s+)+(?:uint32_t|uint32|uint64_t|uint64|int|size_t|std::uint32_t|std::uint64_t|std::size_t)\s+(Hash\w*|hash\w*|NextPow2|RoundUp|AlignUp|IsPow2|Pow2|Wrap|wrap|Clamp|clamp)\s*\([^;]*\)\s*(?:noexcept)?\s*\{?\s*$")
kInlineLerp = re.compile(r"\b(\w+)\s*\+\s*\(\s*(\w+)\s*-\s*\1\s*\)\s*\*\s*\w+")
kInlineClamp = re.compile(r"std::min\s*\(\s*std::max\s*\(|std::max\s*\(\s*std::min\s*\(|\bstd::clamp\s*\([^;]*,\s*0\.0*f?\s*,\s*1\.0*f?\s*\)")
kInlineSaturate = re.compile(r"\?\s*0\.0*f?\s*:\s*[^;]*\?\s*1\.0*f?\s*:")
kSaturateStdClamp = re.compile(r"\bstd::clamp\s*\([^;]*,\s*0\.0*f?\s*,\s*1\.0*f?\s*\)")
kInlineSmoothStep = re.compile(r"\b(\w+)\s*\*\s*\1\s*\*\s*\(\s*3\.0*f?\s*-\s*2\.0*f?\s*\*\s*\1\s*\)")
kFloatHash = re.compile(r"12\.9898|43758\.5453|0x5bd1e995|2654435761|1664525|1103515245|0x9E3779B9|0x9e3779b9|0x85ebca6b|0xcc9e2d51|0x27d4eb2d|\bsin\s*\([^;]*\)\s*\*\s*4\d{4}")
kExcludedModules = {"Mathematics", "Noise"}


def _piece_for(function_name):
    name = function_name.lower()
    if name.startswith(("lerp", "mix")):
        return "Mathematics::Lerp (Mathematics/Interpolation.h)"
    if "inverselerp" in name:
        return "Mathematics::InverseLerp (Mathematics/Interpolation.h)"
    if name.startswith(("clamp01", "saturate")):
        return "Mathematics::Clamp01 (Mathematics/Interpolation.h)"
    if name.startswith("clamp"):
        return "std::clamp"
    if name.startswith(("remap", "maprange")):
        return "Mathematics::Remap (Mathematics/Interpolation.h)"
    if "smooth" in name:
        return "Mathematics::SmoothStep (Mathematics/Interpolation.h)"
    if name.startswith("ease"):
        return "Mathematics/Easing.h"
    if name.startswith("hash"):
        return "Fnv1a64 or Fnv1a64Value (Types/Fnv1a.h), or the Noise module's hash for grids"
    if name.startswith(("noise", "perlin", "simplex", "fbm", "worley", "voronoi")):
        return "the Noise module (GradientNoise2D, FractalNoise2D, GradientCurl3D)"
    if name in ("radians", "degrees", "toradians", "todegrees", "degtorad", "radtodeg"):
        return "Mathematics/Angle.h"
    if name.startswith(("fract", "frac", "wrap", "step", "sign")):
        return "a Fract or Wrap in Mathematics/Interpolation.h (to add), or std::fmod and std::copysign"
    if name in ("approximately", "nearlyequal", "almostequal"):
        return "Mathematics"
    if name in ("pow2", "nextpow2", "ispow2", "roundup", "alignup"):
        return "std::bit_ceil or std::has_single_bit"
    return "Mathematics"


class Rule:
    def __init__(self, files):
        pass

    def scan_file(self, source):
        if source.module in kExcludedModules:
            return []
        path = source.path
        sites = []
        for line_number, code, _ in source.code:
            if not code.strip():
                continue
            code = strip_strings(code)
            definition = kLocalDefinition.match(code) or kLocalDefinitionInteger.match(code)
            if definition:
                sites.append(Site(path, line_number, "local_definition", _piece_for(definition.group(1))))
                continue
            if kInlineSmoothStep.search(code):
                sites.append(Site(path, line_number, "inline_smoothstep", "Mathematics::SmoothStep (Mathematics/Interpolation.h)"))
            elif kInlineSaturate.search(code):
                sites.append(Site(path, line_number, "inline_saturate_ternary", "Mathematics::Clamp01 (Mathematics/Interpolation.h)"))
            elif kSaturateStdClamp.search(code):
                sites.append(Site(path, line_number, "saturate_std_clamp01", "Mathematics::Clamp01 (Mathematics/Interpolation.h)"))
            elif kInlineClamp.search(code):
                sites.append(Site(path, line_number, "inline_clamp_minmax", "std::clamp"))
            if kInlineLerp.search(code) and "Lerp" not in code and "lerp" not in code:
                sites.append(Site(path, line_number, "inline_lerp", "Mathematics::Lerp (Mathematics/Interpolation.h)"))
            if kFloatHash.search(code):
                sites.append(Site(path, line_number, "float_hash_constant", "Fnv1a64, Fnv1a64Value or HashCombine (Types/Fnv1a.h), or the Noise module's hash"))
        return sites

    def finish(self):
        return []
