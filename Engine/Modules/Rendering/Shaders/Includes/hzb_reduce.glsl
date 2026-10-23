// The 2x2 reduction of the depth pyramids. Reverse-Z: near = 1, far = 0.
//  - Default, the occlusion pyramid (View.HZB): MIN keeps the FARTHEST depth of
//    each footprint. Culling must be conservative: an instance is culled only
//    when it lies behind the farthest occluder depth of the tiles it covers.
//  - HZB_REDUCE_NEAREST, the reflection traversal pyramid (View.SSRHiZ): MAX
//    keeps the NEAREST depth. A ray registers a crossing as soon as it could
//    reach the nearest surface in a tile, so the march never steps over a hit.
#ifdef HZB_REDUCE_NEAREST
#define HZB_REDUCE(a, b) max(a, b)
#else
#define HZB_REDUCE(a, b) min(a, b)
#endif
