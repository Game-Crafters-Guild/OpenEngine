"""Class 01, math types: raw float arrays, std::array of floats, float pointers that carry a math quantity, tuples of floats.

The owners of the engine types (Mathematics, Types) are out of scope. A (float x, float y, float z) signature is counted by the
inventory but not gated: it is an API shape, judged per signature.
"""
import re

from gate_common import Site, strip_strings

CLASS_ID = "01_math_types"
PATTERNS = ["raw_array", "std_array", "float_ptr", "tuple"]

kFloat = r"(?:float|double|float32|float64)"
kSizes = r"(?:2|3|4|9|16)"
kRawArray = re.compile(rf"\b{kFloat}\s+(?:const\s+)?(\w+)\s*\[\s*({kSizes})\s*\]")
kStdArray = re.compile(rf"std::array\s*<\s*{kFloat}\s*,\s*({kSizes})\s*>\s*(\w*)")
kMathName = re.compile(
    r"(pos|position|dir|direction|color|colour|rgb|rgba|albedo|tint|emissive|mat|matrix|xyz|normal|tangent|bitangent|scale|rot|rotation|quat|orientation|translation|bounds|aabb|extent|extents|origin|center|centre|axis|up|forward|right|vertex|vertices|verts|point|points|pt|uv|texcoord|offset|velocity|accel|min|max|half|size|corner|eye|target|lookat|euler|angles|pivot|anchor|world|local|view|proj|projection|mvp|transform|xform|basis|frustum|plane|ray|dims|dimension|resolution|coords|coord|radiance|irradiance|specular|diffuse|ambient|fog|sun|light|sky)",
    re.IGNORECASE,
)
kFloatPointer = re.compile(rf"\b(?:const\s+)?{kFloat}\s*\*\s*(?:const\s+)?(\w+)")
kTuple = re.compile(rf"std::(?:tuple|pair)\s*<\s*{kFloat}(?:\s*,\s*{kFloat}){{1,3}}\s*>")
kColorName = re.compile(r"color|colour|rgb|albedo|tint|emissive|radiance|irradiance|specular|diffuse|ambient", re.IGNORECASE)
kQuaternionName = re.compile(r"quat|rot|orientation", re.IGNORECASE)
kBoundsName = re.compile(r"bounds|aabb|extent|min|max|corner", re.IGNORECASE)
kMatrixName = re.compile(r"mat|matrix|xform|transform|view|proj|mvp|basis", re.IGNORECASE)
kRectName = re.compile(r"rect|viewport|scissor", re.IGNORECASE)
kPlaneName = re.compile(r"plane", re.IGNORECASE)
kPointsName = re.compile(r"vertices|verts|points|coords", re.IGNORECASE)
kExcludedModules = {"Mathematics", "Types"}


def _piece_for_array(size, name):
    if size == 2:
        return "Mathematics::Vector2"
    if size == 3:
        return "ColorLinear (Types/Color.h), rgb" if kColorName.search(name) else "Mathematics::Vector3"
    if size == 4:
        if kColorName.search(name):
            return "ColorLinear (Types/Color.h)"
        if kQuaternionName.search(name):
            return "Mathematics::Quaternion"
        if kPlaneName.search(name):
            return "Mathematics::Plane"
        if kRectName.search(name):
            return "Mathematics::Rect"
        return "Mathematics::Vector4"
    if size == 9:
        return "Mathematics::Matrix3x3"
    return "Mathematics::Matrix4x4"


def _piece_for_pointer(name):
    if kMatrixName.search(name):
        return "const Mathematics::Matrix4x4&"
    if kColorName.search(name):
        return "const ColorLinear& (Types/Color.h)"
    if kQuaternionName.search(name):
        return "const Mathematics::Quaternion&"
    if kBoundsName.search(name):
        return "const Mathematics::BoundingBox&"
    if kPointsName.search(name):
        return "std::span<const Mathematics::Vector3>"
    return "const Mathematics::Vector3& (or Vector2, Vector4)"


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
            code = strip_strings(code)
            for match in kRawArray.finditer(code):
                sites.append(Site(source.path, line_number, "raw_array", _piece_for_array(int(match.group(2)), match.group(1))))
            for match in kStdArray.finditer(code):
                sites.append(Site(source.path, line_number, "std_array", _piece_for_array(int(match.group(1)), match.group(2))))
            for match in kFloatPointer.finditer(code):
                if kMathName.search(match.group(1)):
                    sites.append(Site(source.path, line_number, "float_ptr", _piece_for_pointer(match.group(1))))
            for _ in kTuple.finditer(code):
                sites.append(Site(source.path, line_number, "tuple", "Mathematics::Vector2 or Vector3"))
        return sites

    def finish(self):
        return []
