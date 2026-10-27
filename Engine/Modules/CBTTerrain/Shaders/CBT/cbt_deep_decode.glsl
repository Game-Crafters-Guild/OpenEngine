// cbt_deep_decode.glsl — GLSL twin of Include/CBTTerrain/CBTDeepDecode.h (Earth-scale
// decode precision, arc S2a). The two files are edited in LOCKSTEP, the CBTLayout.h <->
// cbt_layout.glsl discipline: every function here mirrors its CPU namesake op-for-op, and
// the contract is BIT-EQUALITY, held by the GPU readback probe
// (CBTDeepDecodeGpu.GpuDecodeMatchesCpuLibraryBitExact) — not just tolerance.
//
// What makes bit-parity possible (and what NOT to touch):
//   * Every fp32 op used is IEEE-correctly-rounded on both sides: add/sub/mul (Vulkan
//     requires correct rounding for FAdd/FSub/FMul) and fma (a single correctly rounded
//     fused op under NoContraction). The two op classes Vulkan does NOT bound to correct
//     rounding are avoided: no division (the one reciprocal, invScale, is an exact power
//     of two constructed from exponent bits) and no inversesqrt() (the 1/sqrt seed is the
//     shared deterministic bit-pattern seed CBT_RSqrtSeed, three fp32 Newton polishes).
//   * `precise` (SPIR-V NoContraction) on every intermediate: the Dekker/Knuth error-free
//     transforms are DESTROYED by fp contraction (an auto-fused a*b+c makes TwoProd's
//     error term 0) and by reassociation. The CPU header pins the same via fp_contract.
//   * floor(x + 0.5) is the shared round-to-sector tie convention (GLSL round()'s
//     half-tie is implementation-defined; std::roundf half-away differs at negative ties).
//
// Compute-only (int64 numerators): guarded on CBT_DECODE_INT64 like the cbt_domain.glsl
// decode functions — the graphics stages read the stored (sector, local) instead.
#ifndef CBT_DEEP_DECODE_GLSL
#define CBT_DEEP_DECODE_GLSL

#ifdef CBT_DECODE_INT64

// Deep-store subdivision cap (mirror DeepDecode::kDeepDecodeSubdiv; locked by
// CBTLayoutTests.GlslDeepDecodeMirrorsCpp). VertexEval's decode guard uses it when
// pc.deepDecode is set; S2b lifted the provisioning/classification cap to the same 50
// (TerrainProvisioning SubdivCapFor, flag-coupled — 40 when the flag is off).
const uint CBT_DEEP_NUM_SUBDIV = 50u;

// Exact 1/1024 (mirror DeepDecode kInvSector; CBT_SECTOR_SIZE lives in cbt_layout.glsl).
const float CBT_DEEP_INV_SECTOR = 1.0 / 1024.0;

// GE_SHARED_EQUAL_ANGLE_BEGIN
// The block from here to GE_SHARED_EQUAL_ANGLE_END is compiled as C++ by TerrainAtlasTests
// (GlslShim.h) and swept against the CPU map: float literals suffixed `f`, no swizzles. It reads
// CBTPagePoint from cbt_page.glsl.

// df64 ("double-float"): an unevaluated fp32 pair Hi + Lo, |Lo| <= 0.5 ulp(Hi). Mirror of
// DeepDecode::DF.
struct CBTDf
{
    float Hi;
    float Lo;
};

// Knuth TwoSum: s + err == a + b exactly. Branch-free. Mirror of DeepDecode::TwoSum.
CBTDf CBT_DfTwoSum(float a, float b)
{
    precise float s = a + b;
    precise float bb = s - a;
    precise float err = (a - (s - bb)) + (b - bb);
    return CBTDf(s, err);
}

// Dekker FastTwoSum: requires |a| >= |b|. Mirror of DeepDecode::FastTwoSum.
CBTDf CBT_DfFastTwoSum(float a, float b)
{
    precise float s = a + b;
    precise float err = b - (s - a);
    return CBTDf(s, err);
}

// TwoProd via FMA: p + err == a * b exactly. Mirror of DeepDecode::TwoProd.
CBTDf CBT_DfTwoProd(float a, float b)
{
    precise float p = a * b;
    precise float err = fma(a, b, -p);
    return CBTDf(p, err);
}

CBTDf CBT_DfAdd(CBTDf a, CBTDf b)
{
    CBTDf s = CBT_DfTwoSum(a.Hi, b.Hi);
    precise float lo = s.Lo + (a.Lo + b.Lo);
    return CBT_DfFastTwoSum(s.Hi, lo);
}

CBTDf CBT_DfMul(CBTDf a, CBTDf b)
{
    CBTDf p = CBT_DfTwoProd(a.Hi, b.Hi);
    precise float lo = p.Lo + (a.Hi * b.Lo + a.Lo * b.Hi);
    return CBT_DfFastTwoSum(p.Hi, lo);
}

CBTDf CBT_DfMulF(CBTDf a, float b)
{
    CBTDf p = CBT_DfTwoProd(a.Hi, b);
    precise float lo = p.Lo + a.Lo * b;
    return CBT_DfFastTwoSum(p.Hi, lo);
}

// int64 -> df64 (deterministic, exactly power-of-two-scale invariant; relative error
// <= ~2^-47). ConvertSToF is correctly rounded RTE, ConvertFToS truncates — both sides.
CBTDf CBT_DfFromI64(int64_t v)
{
    float hi = float(v);
    float lo = float(v - int64_t(hi));
    return CBTDf(hi, lo);
}

// r(t) of the page store's equal-angle map (mirror PageStreaming::EqualAngleFaceCoordinate,
// PageStoreFormat.cpp): each float64 coefficient of the cubic as an fp32 pair.
CBTDf CBT_EqualAngleRemainder(CBTDf t)
{
    CBTDf r = CBT_DfAdd(CBT_DfMul(t, CBTDf(0.02328246831893921f, -5.819869586254356e-10f)),
                        CBTDf(-0.07874228805303574f, -3.2064537691667283e-09f));
    r = CBT_DfAdd(CBT_DfMul(t, r), CBTDf(0.14697904884815216f, -2.8555793285534037e-09f));
    return CBT_DfAdd(CBT_DfMul(t, r), CBTDf(-0.27312028408050537f, -1.334704879951687e-08f));
}

// A position along one axis of a page store face: a level-0 sample and the offset past it in [0, 1).
struct CBTFaceTexel
{
    uint Texel;
    float Fraction;
};

// The equal-angle texel of a = numerator / 2^numSubdiv (exact) on a face of `samples` samples a side, in df64:
// p(a) = a + a (a^2 - 1) r(a^2), sample i at p = (2 i - (samples - 1)) / (samples - 1); a = -1 and 1 have fraction 0.
// Within an fp32 ulp of a texel boundary the result is the same texel, or the next texel with fraction 0.
CBTFaceTexel CBT_EqualAngleTexel(int64_t numerator, uint numSubdiv, uint samples)
{
    CBTDf a = CBT_DfMulF(CBT_DfFromI64(numerator), ldexp(1.0f, -int(numSubdiv)));
    CBTDf t = CBT_DfMul(a, a);
    CBTDf p = CBT_DfAdd(a, CBT_DfMul(CBT_DfMul(a, CBT_DfAdd(t, CBTDf(-1.0f, 0.0f))), CBT_EqualAngleRemainder(t)));
    float last = float(samples - 1u);
    CBTDf x = CBT_DfMulF(CBT_DfAdd(p, CBTDf(1.0f, 0.0f)), last * 0.5f);
    // Hi - floor(Hi) is exact; Lo moves the sum at most one texel either way.
    precise float whole = floor(x.Hi);
    precise float rest = (x.Hi - whole) + x.Lo;
    precise float carry = floor(rest);
    whole = whole + carry;
    rest = rest - carry;
    if (rest >= 1.0f)
    {
        whole = whole + 1.0f;
        rest = 0.0f;
    }
    if (whole < 0.0f)
    {
        whole = 0.0f;
        rest = 0.0f;
    }
    if (whole >= last)
    {
        whole = last;
        rest = 0.0f;
    }
    CBTFaceTexel result;
    result.Texel = uint(whole);
    result.Fraction = rest;
    return result;
}

// The page store point of cube point (nx, ny, nz) / 2^numSubdiv of a root on face `rootFace`, `samples` a face side.
// A point on a cube edge or corner resolves on the lowest face that holds it (planets 3.12): all roots read one face.
// The face's X and Z axes are those of Mathematics::kCubeFaceAxes, each along +1 on its world axis.
CBTPagePoint CBT_PlanetPagePoint(uint rootFace, int64_t nx, int64_t ny, int64_t nz, uint numSubdiv, uint samples)
{
    int64_t scale = int64_t(1) << numSubdiv;
    uint face = rootFace;
    for (uint lower = 0u; lower < rootFace; ++lower)
    {
        uint axis = lower >> 1u;
        int64_t normal = axis == 0u ? nx : axis == 1u ? ny : nz;
        if (normal == ((lower & 1u) == 0u ? scale : -scale))
        {
            face = lower;
            break;
        }
    }
    int64_t alongX = (face == 0u || face == 5u) ? ny : (face == 1u || face == 2u) ? nz : nx;
    int64_t alongZ = (face == 0u || face == 3u) ? nz : (face == 1u || face == 4u) ? ny : nx;
    CBTFaceTexel x = CBT_EqualAngleTexel(alongX, numSubdiv, samples);
    CBTFaceTexel z = CBT_EqualAngleTexel(alongZ, numSubdiv, samples);
    CBTPagePoint point;
    point.Face = face;
    point.TexelX = x.Texel;
    point.TexelZ = z.Texel;
    point.FractionX = x.Fraction;
    point.FractionZ = z.Fraction;
    return point;
}
// GE_SHARED_EQUAL_ANGLE_END

// Deterministic shared 1/sqrt seed (mirror DeepDecode::RSqrtSeed): exponent-halving bit
// pattern + three fp32 Newton polishes — pure IEEE add/mul, bit-identical CPU<->GPU
// (hardware inversesqrt() is only 2-ULP-bounded on Vulkan and differs from the CPU's).
float CBT_RSqrtSeed(float x)
{
    precise float r = uintBitsToFloat(0x5F3759DFu - (floatBitsToUint(x) >> 1u));
    r = r * (1.5 - 0.5 * x * r * r);
    r = r * (1.5 - 0.5 * x * r * r);
    r = r * (1.5 - 0.5 * x * r * r);
    return r;
}

// df64 1/sqrt: shared seed + one df64 Newton step (~2^-44 relative). Mirror of
// DeepDecode::RSqrt.
CBTDf CBT_DfRSqrt(CBTDf s)
{
    CBTDf rDf = CBTDf(CBT_RSqrtSeed(s.Hi), 0.0);
    CBTDf r2 = CBT_DfMul(rDf, rDf);
    CBTDf sr2 = CBT_DfMul(s, r2);
    CBTDf polish = CBT_DfAdd(CBTDf(1.5, 0.0), CBT_DfMulF(sr2, -0.5));
    return CBT_DfMul(rDf, polish);
}

// One decoded corner: integer world sector (1024 m grid) + fp32 sector-local offset.
// Mirror of DeepDecode::DeepVertex; consumption is rel = vec3(sector - originSector) *
// CBT_SECTOR_SIZE + local (GE_ClipFromSectorLocal math, exact integer delta).
struct CBTDeepVertex
{
    ivec3 Sector;
    vec3 Local;
};

// Split one df64 world component into (round-to-nearest sector, local remainder).
// k * CBT_SECTOR_SIZE is exact while |sector| <= 16384 (|world| < 2^24 m — the same
// engine-wide render-origin exactness bound CBT_RenderOriginWorld documents).
void CBT_DeepSplitComponent(CBTDf w, out int outSector, out float outLocal)
{
    precise float k = floor(w.Hi * CBT_DEEP_INV_SECTOR + 0.5);
    precise float negSector = -k * CBT_SECTOR_SIZE;
    CBTDf local = CBT_DfAdd(w, CBTDf(negSector, 0.0));
    precise float l = local.Hi + local.Lo; // |local| <= 512 -> one fp32 holds it to ~6e-5 m
    outSector = int(k);
    outLocal = l;
}

// The deep decode: exact int64 cube numerator N (|N| <= 2^numSubdiv) -> (sector, local).
// dir = N/|N| in df64, world = dir * (radius + relief) in df64, then the per-component
// sector split. Pure function of (N, numSubdiv, radius, relief) — bit-identical across
// every bisector sharing the corner (the crack-freedom contract; DeepDecode::DecodeCorner
// is the CPU authority this must match bit-for-bit).
CBTDeepVertex CBT_DeepDecodeCorner(i64vec3 n, uint numSubdiv, float radiusMeters,
                                   float reliefMeters)
{
    CBTDf nx = CBT_DfFromI64(n.x);
    CBTDf ny = CBT_DfFromI64(n.y);
    CBTDf nz = CBT_DfFromI64(n.z);
    // Exact 2^-numSubdiv from exponent bits: fp32 division is NOT correctly-rounded-
    // guaranteed on Vulkan (2.5 ULP), so the reciprocal is constructed, not divided.
    // numSubdiv <= CBT_DEEP_NUM_SUBDIV (50) keeps the biased exponent positive.
    float invScale = uintBitsToFloat((127u - numSubdiv) << 23u);
    // Pre-scale to c = N * 2^-numSubdiv in [-1,1]^3 BEFORE squaring: N^2 would exceed the
    // fp32 exponent range past numSubdiv 63 (mirror of the CPU header's note).
    CBTDf cx = CBT_DfMulF(nx, invScale);
    CBTDf cy = CBT_DfMulF(ny, invScale);
    CBTDf cz = CBT_DfMulF(nz, invScale);
    CBTDf len2 = CBT_DfAdd(CBT_DfAdd(CBT_DfMul(cx, cx), CBT_DfMul(cy, cy)), CBT_DfMul(cz, cz));
    CBTDf rlen = CBT_DfRSqrt(len2);
    CBTDf scaleR = CBT_DfMul(rlen, CBT_DfTwoSum(radiusMeters, reliefMeters));
    CBTDf wx = CBT_DfMul(cx, scaleR);
    CBTDf wy = CBT_DfMul(cy, scaleR);
    CBTDf wz = CBT_DfMul(cz, scaleR);

    CBTDeepVertex outV;
    CBT_DeepSplitComponent(wx, outV.Sector.x, outV.Local.x);
    CBT_DeepSplitComponent(wy, outV.Sector.y, outV.Local.y);
    CBT_DeepSplitComponent(wz, outV.Sector.z, outV.Local.z);
    return outV;
}

#endif // CBT_DECODE_INT64

#endif // CBT_DEEP_DECODE_GLSL
