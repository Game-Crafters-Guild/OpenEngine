// cbt_planar_demand.glsl: the planar split demand of a CBT node, from the node's 3D box.
//
// A node's box is its UV bounding box in XZ, with the height pyramid's range over that box in Y,
// in the render-origin-relative frame. A child's triangle lies inside its parent's, so its box lies
// inside its parent's box, and every quantity below is a bound over the box that can only shrink
// (a size, a range, a tangent) or only grow (a depth, a distance) on a sub-box. So a child's demand
// is never above its parent's, and a veto on the parent holds on the child: the demand is monotone
// in depth by construction. With one threshold, the tree a pose asks for is then one tree, whatever
// path the camera took (Kernel_Classify).
//
// The demand is the node's size, sqrt(E^2 + r^2), over a view depth bound of its box, in pixels:
//   E   the undisplaced split edge (meters);
//   r   the height range of the surface the draw shows over the box (meters): the level-0 range near
//       the eye (bounded by the steepest step times the box's extent below the pyramid's block size,
//       CBT_PlanarStepBoundRange), the smaller range of the prefiltered chain level the draw reads
//       farther out;
//   D   max(least view depth of the box's corners, kappa x the box distance, 1 m), where kappa is the
//       cosine of the half-diagonal field of view (no point in view sits at a larger angle);
//   the off-axis stretch sqrt(1 + tx^2 + ty^2) of the wide-screen projection, tx and ty the box's
//   largest view tangents, clamped to the margin frustum's.
// A box wholly outside the margin frustum, or behind the eye, takes the isotropic form
// size x f / max(distance, 1 m), which is never above the in-frustum form of the same box.
// The flat veto: a node whose drawn range projects at most flatPx at the box's nearest point does
// not split, because its children would draw the same plane.
//
// The block between the GE_SHARED_PLANAR_DEMAND markers is compiled as C++ by CBTTerrainTests
// (GlslShim.h): written component-wise, float literals suffixed `f`, no out parameters. Pure math, no
// bindings. Every operation is precise (no fused multiply-add), so the copies Kernel_Classify inlines
// for a node, its parent and its twin compute the same bits for the same box.
#ifndef CBT_PLANAR_DEMAND_GLSL
#define CBT_PLANAR_DEMAND_GLSL

// GE_SHARED_PLANAR_DEMAND_BEGIN
// The view the planar demand reads, from the render-origin-relative view-projection.
struct CBTPlanarView
{
    vec4 RowX;         // clip x as a function of (position, 1)
    vec4 RowY;         // clip y
    vec4 RowW;         // clip w: the view depth under a perspective camera
    float FocalPx;     // the vertical focal length in pixels
    float ViewPerClipX; // view x per clip x (1 / P00)
    float ViewPerClipY; // view y per clip y (1 / P11)
    float Kappa;       // the cosine of the half-diagonal field of view
    float MarginTanX;  // the margin frustum's half tangents
    float MarginTanY;
    float Margin;      // the margin frustum's NDC extent (CBT_SPLIT_NDC_MARGIN)
    uint Ortho;        // 1 for an orthographic camera: the demand is the size at the fixed scale
};

// The least value of row . (p, 1) over the box [lo, hi]: a linear function takes it at a corner.
float CBT_BoxRowMin(vec4 row, vec3 lo, vec3 hi)
{
    precise float v = row.w + min(row.x * lo.x, row.x * hi.x) + min(row.y * lo.y, row.y * hi.y) +
                      min(row.z * lo.z, row.z * hi.z);
    return v;
}

// The greatest value of row . (p, 1) over the box [lo, hi].
float CBT_BoxRowMax(vec4 row, vec3 lo, vec3 hi)
{
    precise float v = row.w + max(row.x * lo.x, row.x * hi.x) + max(row.y * lo.y, row.y * hi.y) +
                      max(row.z * lo.z, row.z * hi.z);
    return v;
}

// The distance from `eye` to the box [lo, hi]: zero inside.
float CBT_DistanceToBox(vec3 lo, vec3 hi, vec3 eye)
{
    precise float dx = max(max(lo.x - eye.x, eye.x - hi.x), 0.0f);
    precise float dy = max(max(lo.y - eye.y, eye.y - hi.y), 0.0f);
    precise float dz = max(max(lo.z - eye.z, eye.z - hi.z), 0.0f);
    precise float dist = sqrt(dx * dx + dy * dy + dz * dz);
    return dist;
}

// The distance from `eye` to the farthest corner of the box [lo, hi].
float CBT_FarthestOfBox(vec3 lo, vec3 hi, vec3 eye)
{
    precise float dx = max(abs(lo.x - eye.x), abs(hi.x - eye.x));
    precise float dy = max(abs(lo.y - eye.y), abs(hi.y - eye.y));
    precise float dz = max(abs(lo.z - eye.z), abs(hi.z - eye.z));
    precise float dist = sqrt(dx * dx + dy * dy + dz * dz);
    return dist;
}

// Whether the box lies wholly behind the eye or wholly outside one side of the margin frustum
// (|x| <= margin x w, |y| <= margin x w, w > 0 in clip space). A sub-box of such a box is too.
bool CBT_BoxOutsideMarginFrustum(CBTPlanarView v, vec3 lo, vec3 hi)
{
    if (v.Ortho != 0u)
        return false;
    if (CBT_BoxRowMax(v.RowW, lo, hi) <= 1.0e-5f)
        return true;
    float m = v.Margin;
    vec4 right = vec4(m * v.RowW.x - v.RowX.x, m * v.RowW.y - v.RowX.y, m * v.RowW.z - v.RowX.z,
                      m * v.RowW.w - v.RowX.w);
    vec4 left = vec4(m * v.RowW.x + v.RowX.x, m * v.RowW.y + v.RowX.y, m * v.RowW.z + v.RowX.z,
                     m * v.RowW.w + v.RowX.w);
    vec4 top = vec4(m * v.RowW.x - v.RowY.x, m * v.RowW.y - v.RowY.y, m * v.RowW.z - v.RowY.z,
                    m * v.RowW.w - v.RowY.w);
    vec4 bottom = vec4(m * v.RowW.x + v.RowY.x, m * v.RowW.y + v.RowY.y, m * v.RowW.z + v.RowY.z,
                       m * v.RowW.w + v.RowY.w);
    return CBT_BoxRowMax(right, lo, hi) < 0.0f || CBT_BoxRowMax(left, lo, hi) < 0.0f ||
           CBT_BoxRowMax(top, lo, hi) < 0.0f || CBT_BoxRowMax(bottom, lo, hi) < 0.0f;
}

// The view depth bound D of the box (meters), `dist` its distance from the eye and `outside` its
// CBT_BoxOutsideMarginFrustum answer: max(least corner depth, kappa x dist, 1 m) in the frustum,
// max(dist, 1 m) outside it. It never shrinks on a sub-box, across the two forms included.
float CBT_PlanarBoxDepth(CBTPlanarView v, vec3 lo, vec3 hi, float dist, bool outside)
{
    if (v.Ortho != 0u)
        return 1.0f;
    if (outside)
        return max(dist, 1.0f);
    precise float kappaDist = v.Kappa * dist;
    precise float depth = max(max(CBT_BoxRowMin(v.RowW, lo, hi), kappaDist), 1.0f);
    return depth;
}

// The demand of a node (pixels): `sizeM` its size sqrt(E^2 + r^2), `depth` its CBT_PlanarBoxDepth.
float CBT_PlanarBoxDemand(CBTPlanarView v, vec3 lo, vec3 hi, float depth, bool outside, float sizeM)
{
    if (v.Ortho != 0u)
    {
        precise float scaled = sizeM * v.FocalPx;
        return scaled;
    }
    if (outside)
    {
        precise float isotropic = sizeM * v.FocalPx / depth;
        return isotropic;
    }
    precise float xMax = max(abs(CBT_BoxRowMin(v.RowX, lo, hi)), abs(CBT_BoxRowMax(v.RowX, lo, hi))) * v.ViewPerClipX;
    precise float yMax = max(abs(CBT_BoxRowMin(v.RowY, lo, hi)), abs(CBT_BoxRowMax(v.RowY, lo, hi))) * v.ViewPerClipY;
    precise float tx = min(xMax / depth, v.MarginTanX);
    precise float ty = min(yMax / depth, v.MarginTanY);
    precise float demand = sizeM * v.FocalPx * sqrt(1.0f + tx * tx + ty * ty) / depth;
    return demand;
}

// The node's size: the undisplaced split edge `edgeM` and the drawn range `rangeM` (meters).
float CBT_PlanarNodeSize(float edgeM, float rangeM)
{
    precise float size = sqrt(edgeM * edgeM + rangeM * rangeM);
    return size;
}

// The range of the surface over a box (meters): `rangeM`, the range of the pyramid blocks that cover
// it, or `stepM` x (`cellsX` + `cellsZ`) when that is smaller, where stepM is the steepest step between
// two neighboring lattice samples in those blocks and cellsX, cellsZ the box's extent in lattice
// cells. The surface between the samples is bilinear, so along X it changes by at most the steepest X
// step per cell, along Z likewise: any two points of the box differ by at most stepM times their X
// distance plus their Z distance, in cells. On a sub-box the blocks, the step and the extents only
// shrink, so the bound nests: a box smaller than one block keeps a range that shrinks with it instead
// of the whole block's.
float CBT_PlanarStepBoundRange(float rangeM, float stepM, float cellsX, float cellsZ)
{
    precise float stepRange = stepM * (cellsX + cellsZ);
    return min(rangeM, stepRange);
}

// The flat veto: whether the drawn range `rangeM` projects at most `flatPx` at the box's nearest
// point, `dist` from the eye. Off when flatPx is 0.
bool CBT_PlanarBoxFlat(CBTPlanarView v, float rangeM, float dist, float flatPx)
{
    if (!(flatPx > 0.0f))
        return false;
    precise float reach = v.Ortho != 0u ? 1.0f : max(dist, 1.0f);
    precise float rangePx = rangeM * v.FocalPx / reach;
    return rangePx <= flatPx;
}

// The chain levels whose range bounds the surface the draw shows over a box whose nearest point sits
// at continuous level `lambdaNear` (CBT_ChainLevel): every level from 1 up to floor(lambdaNear),
// because each drawn vertex there reads that level or a coarser one, and a coarser level is a convex
// combination of a finer one. 0 while a vertex of the box can still read level 0 (lambda under 1:
// level 0 alone up to 0.75, blended toward level 1 above). At most `stored`.
uint CBT_DrawnRangeChainLevels(float lambdaNear, uint stored)
{
    if (!(lambdaNear >= 1.0f))
        return 0u;
    return min(uint(min(lambdaNear, float(stored))), stored);
}
// GE_SHARED_PLANAR_DEMAND_END

#endif // CBT_PLANAR_DEMAND_GLSL
