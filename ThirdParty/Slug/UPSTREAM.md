# Slug

- Source: https://github.com/EricLengyel/Slug
- Upstream revision: not recorded; ported on 2026-04-12. The upstream's
  `SlugPixelShader.hlsl` last changed at `fe24e8d7a442fb345c83e4217f7ed18e9c9aa785`
  (2026-03-19). `LICENSE-MIT` and `NOTICE` are copied verbatim from that revision.
- License: the upstream offers its code under MIT or Apache-2.0, and this copy
  is taken under MIT (see `LICENSE-MIT` and `NOTICE`).
- Ported components: the per-pixel coverage evaluation of
  `SlugPixelShader.hlsl` (`CalcRootCode`, `SolveHorizPoly`, `SolveVertPoly`,
  `CalcBandLoc`, `CalcCoverage` and `SlugRender`), as GLSL in
  `Engine/Modules/Rendering/Shaders/UI/slug_functions.glsl`.
- Changes: the per-crossing coverage ramp is the exact-area ramp convolved with
  a box filter (`slugFilteredRamp`); the band loops clamp the curve count that
  they fetch from the band texture; the outline distance functions
  (`slugCurveDistance`, `slugOutlineDistance`) and the curve and band texture
  packing in `Engine/Modules/Text` are the engine's own.
