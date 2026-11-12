// cbt_planet_page.glsl: the planet fragment's height and height gradient from its height pages (planets 3.6,
// 3.7): the fragment's face and level-0 lattice point (the face's anchor, split on the CPU in float64, plus an
// fp32 offset from the fragment's position less the eye's), then a central difference of the paged height.
//
// TerrainAtlasTests compiles the block between the GE_SHARED_PLANET_PAGE markers as C++ (GlslShim.h): write it
// component-wise, float literals suffixed `f`, no out parameters. Requires cbt_page.glsl and its five taps.
#ifndef CBT_PLANET_PAGE_GLSL
#define CBT_PLANET_PAGE_GLSL

// GE_SHARED_PLANET_PAGE_BEGIN
// The equal-angle map p(a) = a + a (a^2 - 1) r(a^2) of cbt_deep_decode.glsl as one odd polynomial:
// p(a) = K1 a + K3 a^3 + K5 a^5 + K7 a^7 + K9 a^9 (mirror PageStreaming::EqualAngleFaceCoordinate).
const float CBT_EQUAL_ANGLE_K1 = 1.2731202841f;
const float CBT_EQUAL_ANGLE_K3 = -0.42009933293f;
const float CBT_EQUAL_ANGLE_K5 = 0.2257213369f;
const float CBT_EQUAL_ANGLE_K7 = -0.10202475637f;
const float CBT_EQUAL_ANGLE_K9 = 0.023282468319f;
// Mirror CBTLayout.h kCBTPlanetPageNoAnchor.
const int CBT_PLANET_PAGE_NO_ANCHOR = 2147483647;

// The height of a planet's page field at a fragment and its gradient on the unit sphere (meters per
// radian, tangential to the fragment's direction).
struct CBTPlanetPageSurface
{
    float Height;
    vec3 Gradient;
};

// A fragment's place on its face's level-0 lattice: the anchor's texel plus an fp32 offset, the gnomonic
// coordinates, and the page point of the center.
struct CBTPlanetPagePoint
{
    uint Face;
    int OriginX;
    int OriginZ;
    float OffsetX;
    float OffsetZ;
    float AX;
    float AZ;
    CBTPagePoint Center;
};

// (p(a) - p(b)) / (a - b) of the equal-angle map, exact as a polynomial: p'(a) when b equals a.
// d_n = (a^n - b^n) / (a - b) = a d_(n-1) + b^(n-1).
float CBT_EqualAngleDividedDifference(float a, float b)
{
    float b2 = b * b;
    float b4 = b2 * b2;
    float d2 = a + b;
    float d3 = a * d2 + b2;
    float d4 = a * d3 + b2 * b;
    float d5 = a * d4 + b4;
    float d6 = a * d5 + b4 * b;
    float d7 = a * d6 + b4 * b2;
    float d8 = a * d7 + b4 * b2 * b;
    float d9 = a * d8 + b4 * b4;
    return CBT_EQUAL_ANGLE_K1 + CBT_EQUAL_ANGLE_K3 * d3 + CBT_EQUAL_ANGLE_K5 * d5 + CBT_EQUAL_ANGLE_K7 * d7 +
           CBT_EQUAL_ANGLE_K9 * d9;
}

// Component `axis` (0 X, 1 Y, 2 Z) of v.
float CBT_PlanetAxis(vec3 v, uint axis)
{
    return axis == 0u ? v.x : (axis == 1u ? v.y : v.z);
}

// The unit vector along world axis `axis`.
vec3 CBT_PlanetAxisVector(uint axis)
{
    return vec3(axis == 0u ? 1.0f : 0.0f, axis == 1u ? 1.0f : 0.0f, axis == 2u ? 1.0f : 0.0f);
}

// The cube face (Mathematics::kCubeFaces order: +X, -X, +Y, -Y, +Z, -Z) whose direction is nearest p.
uint CBT_PlanetFaceOf(vec3 p)
{
    float ax = abs(p.x);
    float ay = abs(p.y);
    float az = abs(p.z);
    uint axis = (ax >= ay && ax >= az) ? 0u : (ay >= az ? 1u : 2u);
    return axis * 2u + (CBT_PlanetAxis(p, axis) < 0.0f ? 1u : 0u);
}

// The world axes of a face's X and Z (mirror CBT_PlanetPagePoint: each along +1 on its world axis).
uint CBT_PlanetFaceXAxis(uint face)
{
    return (face == 0u || face == 5u) ? 1u : ((face == 1u || face == 2u) ? 2u : 0u);
}

uint CBT_PlanetFaceZAxis(uint face)
{
    return (face == 0u || face == 3u) ? 2u : ((face == 1u || face == 4u) ? 1u : 0u);
}

// The component of v along the outward normal of `face`.
float CBT_PlanetFaceNormal(vec3 v, uint face)
{
    float n = CBT_PlanetAxis(v, face >> 1u);
    return (face & 1u) == 0u ? n : -n;
}

// The lattice coordinate along one face axis, less the anchor's texel, of the point at eye + fromEye.
// With e = eye and r = fromEye along the axis (t) and the face normal (n), the gnomonic coordinates are
// a = (e_t + r_t) / (e_n + r_n) and the eye's b = e_t / e_n, and a - b = (r_t e_n - e_t r_n) / (e_n (e_n + r_n)).
float CBT_PlanetAnchorOffset(float eyeAlong, float eyeNormal, float fromEyeAlong, float fromEyeNormal,
                             float eyeFraction, float halfSpan)
{
    float normal = eyeNormal + fromEyeNormal;
    float a = (eyeAlong + fromEyeAlong) / normal;
    float b = eyeAlong / eyeNormal;
    float delta = (fromEyeAlong * eyeNormal - eyeAlong * fromEyeNormal) / (eyeNormal * normal);
    return eyeFraction + delta * CBT_EqualAngleDividedDifference(a, b) * halfSpan;
}

// The fp32 lattice coordinate along one face axis of gnomonic coordinate a: (p(a) + 1) x halfSpan.
float CBT_PlanetLatticeCoordinate(float a, float halfSpan)
{
    return (a * CBT_EqualAngleDividedDifference(a, 0.0f) + 1.0f) * halfSpan;
}

// Lattice coordinate `origin` + `offset` along one face axis, clamped to the face [0, samples - 1],
// less `origin`.
float CBT_PlanetClampOffset(int origin, float offset, uint samples)
{
    return clamp(offset, float(-origin), float(int(samples) - 1 - origin));
}

// The page point of lattice coordinates (originX + offsetX, originZ + offsetZ) on `face`, inside the face.
CBTPagePoint CBT_PlanetLatticePoint(uint face, int originX, float offsetX, int originZ, float offsetZ, uint samples)
{
    float wholeX = floor(offsetX);
    float wholeZ = floor(offsetZ);
    int texelX = originX + int(wholeX);
    int texelZ = originZ + int(wholeZ);
    int last = int(samples) - 1;
    CBTPagePoint point;
    point.Face = face;
    point.TexelX = uint(min(max(texelX, 0), last));
    point.TexelZ = uint(min(max(texelZ, 0), last));
    point.FractionX = texelX < 0 || texelX >= last ? 0.0f : offsetX - wholeX;
    point.FractionZ = texelZ < 0 || texelZ >= last ? 0.0f : offsetZ - wholeZ;
    return point;
}

// The lattice point of planet-local `position`, `fromEye` from the eye at `eye` (the precise difference),
// on a field of `samples` a face side. The anchor is the fragment's face's (CBT_PLANET_PAGE_NO_ANCHOR in
// anchorX: none). It reads no page.
CBTPlanetPagePoint CBT_PlanetPagePointAt(vec3 position, vec3 fromEye, vec3 eye, int anchorX, int anchorZ,
                                         float eyeFractionX, float eyeFractionZ, uint samples)
{
    float halfSpan = 0.5f * float(samples - 1u);
    uint face = CBT_PlanetFaceOf(position);
    uint xAxis = CBT_PlanetFaceXAxis(face);
    uint zAxis = CBT_PlanetFaceZAxis(face);
    float positionNormal = CBT_PlanetFaceNormal(position, face);
    float aX = CBT_PlanetAxis(position, xAxis) / positionNormal;
    float aZ = CBT_PlanetAxis(position, zAxis) / positionNormal;
    int originX = 0;
    int originZ = 0;
    float offsetX = 0.0f;
    float offsetZ = 0.0f;
    if (anchorX == CBT_PLANET_PAGE_NO_ANCHOR)
    {
        offsetX = CBT_PlanetLatticeCoordinate(aX, halfSpan);
        offsetZ = CBT_PlanetLatticeCoordinate(aZ, halfSpan);
    }
    else
    {
        float eyeNormal = CBT_PlanetFaceNormal(eye, face);
        float fromEyeNormal = CBT_PlanetFaceNormal(fromEye, face);
        originX = anchorX;
        originZ = anchorZ;
        offsetX = CBT_PlanetAnchorOffset(CBT_PlanetAxis(eye, xAxis), eyeNormal, CBT_PlanetAxis(fromEye, xAxis),
                                         fromEyeNormal, eyeFractionX, halfSpan);
        offsetZ = CBT_PlanetAnchorOffset(CBT_PlanetAxis(eye, zAxis), eyeNormal, CBT_PlanetAxis(fromEye, zAxis),
                                         fromEyeNormal, eyeFractionZ, halfSpan);
    }
    CBTPlanetPagePoint at;
    at.Face = face;
    at.OriginX = originX;
    at.OriginZ = originZ;
    at.OffsetX = CBT_PlanetClampOffset(originX, offsetX, samples);
    at.OffsetZ = CBT_PlanetClampOffset(originZ, offsetZ, samples);
    at.AX = aX;
    at.AZ = aZ;
    at.Center = CBT_PlanetLatticePoint(face, originX, at.OffsetX, originZ, at.OffsetZ, samples);
    return at;
}

// The height and its gradient at planet-local `position` (CBT_PlanetPagePointAt's arguments), on `field`
// at level `lambda`: the difference steps 2^lambda level-0 texels, one-sided at a face end.
CBTPlanetPageSurface CBT_PlanetPageSurfaceAt(vec3 position, vec3 fromEye, vec3 eye, int anchorX, int anchorZ,
                                             float eyeFractionX, float eyeFractionZ, float lambda,
                                             CBTPageField field)
{
    uint samples = field.Level0SamplesX;
    float halfSpan = 0.5f * float(samples - 1u);
    CBTPlanetPagePoint at =
        CBT_PlanetPagePointAt(position, fromEye, eye, anchorX, anchorZ, eyeFractionX, eyeFractionZ, samples);
    uint face = at.Face;
    uint xAxis = CBT_PlanetFaceXAxis(face);
    uint zAxis = CBT_PlanetFaceZAxis(face);
    int originX = at.OriginX;
    int originZ = at.OriginZ;
    float offsetX = at.OffsetX;
    float offsetZ = at.OffsetZ;
    float aX = at.AX;
    float aZ = at.AZ;
    float step = exp2(max(lambda, 0.0f));
    float plusX = CBT_PlanetClampOffset(originX, offsetX + step, samples);
    float minusX = CBT_PlanetClampOffset(originX, offsetX - step, samples);
    float plusZ = CBT_PlanetClampOffset(originZ, offsetZ + step, samples);
    float minusZ = CBT_PlanetClampOffset(originZ, offsetZ - step, samples);
    // One locate per fragment: the four taps share the center's walk of the table while they lie in its page
    // (CBT_PageSampleCross); a tap past the page's edge takes its own walk.
    CBTPageCross points;
    points.Center = at.Center;
    points.PlusX = CBT_PlanetLatticePoint(face, originX, plusX, originZ, offsetZ, samples);
    points.MinusX = CBT_PlanetLatticePoint(face, originX, minusX, originZ, offsetZ, samples);
    points.PlusZ = CBT_PlanetLatticePoint(face, originX, offsetX, originZ, plusZ, samples);
    points.MinusZ = CBT_PlanetLatticePoint(face, originX, offsetX, originZ, minusZ, samples);
    CBTPageCrossSample heights = CBT_PageSampleCross(points, lambda, field);
    // Meters per lattice texel along each axis, then per unit of gnomonic coordinate.
    float slopeX = (heights.PlusX - heights.MinusX) / max(plusX - minusX, 1e-6f);
    float slopeZ = (heights.PlusZ - heights.MinusZ) / max(plusZ - minusZ, 1e-6f);
    float perAX = slopeX * halfSpan * CBT_EqualAngleDividedDifference(aX, aX);
    float perAZ = slopeZ * halfSpan * CBT_EqualAngleDividedDifference(aZ, aZ);
    // d a / d direction = (axis - a n) / |direction_n| on the unit sphere: tangential, as a is a ratio.
    vec3 direction = normalize(position);
    float directionNormal = CBT_PlanetFaceNormal(direction, face);
    vec3 outward = CBT_PlanetAxisVector(face >> 1u) * ((face & 1u) == 0u ? 1.0f : -1.0f);
    vec3 gradient = (CBT_PlanetAxisVector(xAxis) - outward * aX) * (perAX / directionNormal) +
                    (CBT_PlanetAxisVector(zAxis) - outward * aZ) * (perAZ / directionNormal);
    CBTPlanetPageSurface surface;
    surface.Height = heights.Center;
    surface.Gradient = gradient - direction * dot(gradient, direction);
    return surface;
}

// The world normal of a surface page's normal sample at unit `direction` on `face`: (normalX, normalZ) are
// its components along the face's tangent frame there (x: the face's X axis made normal to the direction;
// z: x cross the direction), and the rest lies along the direction (mirror PlanetSurfacePageProvider).
vec3 CBT_PlanetSurfaceNormal(float normalX, float normalZ, vec3 direction, uint face)
{
    vec3 faceX = CBT_PlanetAxisVector(CBT_PlanetFaceXAxis(face));
    vec3 tangentX = normalize(faceX - direction * dot(faceX, direction));
    vec3 tangentZ = cross(tangentX, direction);
    float normalY = sqrt(max(1.0f - normalX * normalX - normalZ * normalZ, 0.0f));
    return normalize(tangentX * normalX + direction * normalY + tangentZ * normalZ);
}
// GE_SHARED_PLANET_PAGE_END

#endif // CBT_PLANET_PAGE_GLSL
