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
//   r   how far the surface the draw shows over the box can stand from the facet's own plane (meters):
//       the smallest of the level-0 range (bounded below the pyramid's block size by the steepest step
//       times the box's extent, CBT_PlanarStepBoundRange, and by the second differences times its
//       extent squared, CBT_PlanarCurvatureBound), the width of the slab about a plane that holds
//       the level-0 surface over the box (the plane width of the smallest pyramid block that contains
//       it, CBT_PlaneFoldChild), and farther out the range of the prefiltered chain level the draw reads.
//       Each bounds the distance between the surface over the facet and the facet's plane (a range or a
//       slab that holds the surface holds the facet's corners, so the facet's plane too). The plane
//       width and the curvature bound are zero on a plane of any slope, so r is too;
//   D   max(least view depth of the box's corners, kappa x the box distance, 1 m), where kappa is the
//       cosine of the half-diagonal field of view (no point in view sits at a larger angle);
//   the off-axis stretch sqrt(1 + tx^2 + ty^2) of the wide-screen projection, tx and ty the box's
//   largest view tangents, clamped to the margin frustum's.
// A box wholly outside the margin frustum, or behind the eye, takes the isotropic form
// size x f / max(distance, 1 m), which is never above the in-frustum form of the same box.
// The flat veto: a node whose r projects at most flatPx at the box's nearest point does not split,
// because its children would draw the same plane.
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

// How far a piecewise-linear profile can stand from its chord over a span of `cells` lattice cells
// that crosses `knots` lattice lines, per unit of the largest slope change at a line: 0 with no line
// crossed, else min(knots x cells / 4, (cells^2 + 1) / 8). The profile minus its chord is the sum over
// the crossed lines of the slope change times the span's tent function there, the tent's peak at most
// cells / 4; the tent summed over lines one cell apart is at most its integral, cells^2 / 8, plus 1/8
// for the cell that holds the peak.
float CBT_PlanarChordSpan(float cells, float knots)
{
    if (!(knots > 0.0f))
        return 0.0f;
    precise float few = knots * cells * 0.25f;
    precise float wide = (cells * cells + 1.0f) * 0.125f;
    return min(few, wide);
}

// A bound on the distance between the level-0 surface over a facet and the facet's own plane (meters),
// from the box alone: `secondXM` and `secondZM` the largest second differences along X and Z of the
// lattice samples the box touches (meters per cell squared), `mixedM` their largest mixed difference,
// `cellsX`, `cellsZ` the box's extent in cells and `knotsX`, `knotsZ` the lattice lines strictly inside
// it. The surface is bilinear between the samples: along a line its slope changes only where it crosses
// a lattice line, by at most the second difference there, and inside a cell it curves by the mixed
// difference. A facet with its legs along the axes has three of the box's corners: the surface stands
// at most secondX x span(cellsX) + secondZ x span(cellsZ) from the bilinear patch through the box's
// corners, and that patch at most mixed x cellsX x cellsZ / 4 from the facet's plane. A facet with its
// long edge along an axis has that edge's ends and the opposite side's midpoint: along the long edge
// the facet's plane is the chord, and from there to the apex the segment spans at most half the long
// edge, which adds secondX x span(cellsX / 2) and halves the mixed term. The bound below holds for both
// kinds and either axis, and over a sub-box every input only shrinks, so it nests.
float CBT_PlanarCurvatureBound(float secondXM, float secondZM, float mixedM, float cellsX, float cellsZ,
                               float knotsX, float knotsZ)
{
    precise float alongX =
        secondXM * (CBT_PlanarChordSpan(cellsX, knotsX) + CBT_PlanarChordSpan(0.5f * cellsX, knotsX));
    precise float alongZ =
        secondZM * (CBT_PlanarChordSpan(cellsZ, knotsZ) + CBT_PlanarChordSpan(0.5f * cellsZ, knotsZ));
    precise float twist = mixedM * cellsX * cellsZ * 0.25f;
    return alongX + alongZ + twist;
}

// The interval of the surface's height minus a pyramid block's plane over one of its four children
// (normalized height). `child` is the child's plane and residual (CBTLayout.h kCBTHeightPlaneFirstWord:
// slopes per cell about its center, the residual's low end, its width), `parentSlopeX` and
// `parentSlopeZ` the block's slopes about its own center, `offsetX` and `offsetZ` the child's center
// minus the block's (cells), `halfCells` half the child's width in cells. The two planes differ by a
// linear function: the block's plane at the child's center (`shift`) plus the slope difference times
// the offset from that center, at most `spread` over the child.
vec2 CBT_PlaneFoldChild(vec4 child, float parentSlopeX, float parentSlopeZ, float offsetX, float offsetZ,
                        float halfCells)
{
    precise float spread = (abs(child.x - parentSlopeX) + abs(child.y - parentSlopeZ)) * halfCells;
    precise float shift = parentSlopeX * offsetX + parentSlopeZ * offsetZ;
    precise float low = child.z - spread - shift;
    precise float high = child.z + child.w + spread - shift;
    return vec2(low, high);
}

// A pyramid block's plane and residual from its four children's (CBTLayout.h kCBTHeightPlaneFirstWord):
// the mean of the children's slopes, the union of the children's intervals about that plane
// (CBT_PlaneFoldChild; `offset0` .. `offset3` each child's center minus the block's, in cells), and a
// width never below a child's, so a block's width bounds every block inside it.
vec4 CBT_PlaneFold(vec4 child0, vec4 child1, vec4 child2, vec4 child3, vec2 offset0, vec2 offset1, vec2 offset2,
                   vec2 offset3, float halfCells)
{
    precise float slopeX = 0.25f * (child0.x + child1.x + child2.x + child3.x);
    precise float slopeZ = 0.25f * (child0.y + child1.y + child2.y + child3.y);
    vec2 r0 = CBT_PlaneFoldChild(child0, slopeX, slopeZ, offset0.x, offset0.y, halfCells);
    vec2 r1 = CBT_PlaneFoldChild(child1, slopeX, slopeZ, offset1.x, offset1.y, halfCells);
    vec2 r2 = CBT_PlaneFoldChild(child2, slopeX, slopeZ, offset2.x, offset2.y, halfCells);
    vec2 r3 = CBT_PlaneFoldChild(child3, slopeX, slopeZ, offset3.x, offset3.y, halfCells);
    float low = min(min(r0.x, r1.x), min(r2.x, r3.x));
    float high = max(max(r0.y, r1.y), max(r2.y, r3.y));
    float widest = max(max(child0.w, child1.w), max(child2.w, child3.w));
    precise float width = high - low;
    return vec4(slopeX, slopeZ, low, max(width, widest));
}

// The flat veto: whether r, `rangeM`, projects at most `flatPx` at the box's nearest point, `dist`
// from the eye. Off when flatPx is 0.
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
