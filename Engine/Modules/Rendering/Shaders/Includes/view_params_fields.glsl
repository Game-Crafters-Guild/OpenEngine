// Shared per-view ViewParams std140 field list — the single source for the
// canonical block (view_params.glsl, set 0 binding 7, 720 B), its truncated
// prefixes declared inline by the packaged Forward+ / post-FX shaders, and the
// C++ mirror (Rendering/Core/ViewParamsLayout.h). Field order and std140
// offsets are a GPU ABI; every declaration derives from this list via #include,
// so a change here ripples to all of them and drift cannot compile.
//
// Dual-language X-macro (no include guard — included multiple times by design):
// each consumer #defines GE_VP_MAT4 / GE_VP_VEC4 before including, #undefs after.
//   GLSL:  #define GE_VP_MAT4(n) mat4 n;      #define GE_VP_VEC4(n) vec4 n;
//   C++:   #define GE_VP_MAT4(n) float n[16]; #define GE_VP_VEC4(n) float n[4];
//
// Prefix rule: a shorter consumer declares an exact leading prefix by #defining
// a level guard BEFORE the include. The full 720 B block is the default.
//   GE_VIEWPARAMS_CORE_ONLY -> 176 B (invProj, view, depthTerms, viewRanges, cameraPosWS)
//   GE_VIEWPARAMS_OMIT_PROJ -> 256 B (adds screenSize, viewProj; omits proj)
//   (neither)               -> 720 B (adds proj, mipBiasParams, prevViewProj, invView,
//                                 taaJitter, exposureParams and the relative frame from 496)
//                                 — canonical block + C++ mirror
//
// Order and offsets are shared with every consumer: change them only together with
// ViewParamsLayout.h and the packaged copies of the block.
GE_VP_MAT4(ge_invProj)     //   0  view->clip inverse (unproject to view space)
GE_VP_MAT4(ge_view)        //  64  world->view
GE_VP_VEC4(ge_depthTerms)  // 128  the proj's (m22, m32, m23, m33), read by LinearizeReverseZ
GE_VP_VEC4(ge_viewRanges)  // 144  x=near y=culling range (light grid, background distance)
                           //      z=1/log(y/x) w=timeSeconds
GE_VP_VEC4(ge_cameraPosWS) // 160  xyz=world-space camera position
#ifndef GE_VIEWPARAMS_CORE_ONLY
GE_VP_VEC4(ge_screenSize)  // 176  xy=resolution px, zw=1/resolution
GE_VP_MAT4(ge_viewProj)    // 192  world->clip for this view
#ifndef GE_VIEWPARAMS_OMIT_PROJ
GE_VP_MAT4(ge_proj)        // 256  view->clip (== inverse(ge_invProj))
GE_VP_VEC4(ge_mipBiasParams) // 320 x=material mip bias: log2(renderW/outputW), [-2,0] — TAAU
                             //     texture-sharpness compensation, exactly 0.0 when the view
                             //     is not upscaling. yzw reserved.
GE_VP_MAT4(ge_prevViewProj)  // 336 previous rendered frame's UNJITTERED world->clip, for
                             //     temporal reprojection. Equals ge_viewProj on the first
                             //     frame of a view (zero motion).
GE_VP_MAT4(ge_invView)       // 400 view->world (inverse of ge_view): reconstruct a world
                             //     position without a per-pixel matrix inverse.
GE_VP_VEC4(ge_taaJitter)     // 464 xy=current, zw=previous frame sub-pixel NDC jitter; all
                             //     zero when no jittered AA drives this view. Consumers
                             //     reprojecting through the UNJITTERED ge_prevViewProj add
                             //     zw to land on the previous frame's actual raster grid.
GE_VP_VEC4(ge_exposureParams) // 480 the exposure the tonemap applies to this view: x = the
                             //     Fixed/Manual/Physical linear scale, y = 1 while auto exposure
                             //     meters the view (the metered scale is then in ExposureHistory),
                             //     z = the tonemap's look (tonemap.frag's look values, 3 = no look).
                             //     w reserved. GE_ViewExposureScale (view_exposure.glsl) resolves xy.
// The relative frame: positions relative to the origin of the render origin sector (RenderOrigin.h).
// In sector (0,0,0) the relative view, inverse view and eye hold the bits of their absolute partners;
// the previous view-projection does too when the previous frame was also in sector (0,0,0).
GE_VP_MAT4(ge_viewRel)         // 496 origin-relative->view (CameraData::viewRel)
GE_VP_MAT4(ge_invViewRel)      // 560 view->origin-relative (inverse of ge_viewRel)
GE_VP_MAT4(ge_prevViewProjRel) // 624 previous rendered frame's UNJITTERED origin-relative->clip,
                               //     moved into this frame's sector. Equals the current
                               //     unjittered viewProjRel on the first frame of a view.
GE_VP_VEC4(ge_cameraPosRel)    // 688 xyz = the eye relative to the origin (ge_invViewRel[3]), w = 0
GE_VP_VEC4(ge_renderOrigin)    // 704 xyz = the render origin sector (integers stored as float),
                               //     w = the sector size in meters. Origin = xyz * w.
#endif
#endif
