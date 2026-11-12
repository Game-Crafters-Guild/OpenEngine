#ifndef GE_CLUSTER_PROJECTION_GLSL
#define GE_CLUSTER_PROJECTION_GLSL

// Light-grid geometry in view space, for perspective and orthographic projections.
// ShaderVariantKeyTests compiles this file as C++ through GlslShim.h: keep to the shim's subset
// (no swizzles, float literals with an f suffix).

// The side plane of a light-grid tile, as (normal, offset) with the normal toward `inside`.
// nearCorner is on the tile edge at the near plane; farA and farB are the edge's far corners.
// An orthographic tile gets parallel sides, a perspective tile gets sides through the eye.
vec4 GE_ClusterTileSidePlane(vec3 nearCorner, vec3 farA, vec3 farB, vec3 inside)
{
    vec3 normal = normalize(cross(farA - nearCorner, farB - nearCorner));
    if (dot(normal, inside - nearCorner) < 0.0f) normal = normal * -1.0f;
    return vec4(normal, -dot(normal, nearCorner));
}

// The four side planes of a light-grid tile, each from GE_ClusterTileSidePlane.
struct GE_ClusterTilePlanes
{
    vec4 Left;
    vec4 Right;
    vec4 Top;
    vec4 Bottom;
};

// The side planes of a tile from its view-space corners: the four far corners and the near
// corners at the top left and the bottom right.
GE_ClusterTilePlanes GE_ClusterTilePlanesFromCorners(vec3 farTL, vec3 farTR, vec3 farBL, vec3 farBR,
    vec3 nearTL, vec3 nearBR)
{
    // The mean of the far-face center and the near-diagonal center: a point inside the tile.
    vec3 inside = (farTL + farTR + farBL + farBR) * 0.125f + (nearTL + nearBR) * 0.25f;
    GE_ClusterTilePlanes planes;
    planes.Left = GE_ClusterTileSidePlane(nearTL, farBL, farTL, inside);
    planes.Right = GE_ClusterTileSidePlane(nearBR, farTR, farBR, inside);
    planes.Top = GE_ClusterTileSidePlane(nearTL, farTL, farTR, inside);
    planes.Bottom = GE_ClusterTileSidePlane(nearBR, farBR, farBL, inside);
    return planes;
}

// Signed distance from a view-space point to a plane from GE_ClusterTileSidePlane. Positive inside.
float GE_ClusterPlaneDistance(vec4 plane, vec3 point)
{
    return dot(vec3(plane.x, plane.y, plane.z), point) + plane.w;
}

#endif
