// The Karis weight 1 / (1 + luma) (Karis, "High Quality Temporal Supersampling", 2014). A mean with
// this weight equals the mean after the tonemap x / (1 + luma), carried back to HDR, so one bright
// sample cannot dominate. `lumaScale` is 1.0 at the pre-exposure; the TAA resolve gives 1 / P.
#ifndef GE_KARIS_WEIGHT_GLSL
#define GE_KARIS_WEIGHT_GLSL

// Rec.709 relative luminance of a linear color.
float GE_KarisLuma(vec3 rgb)
{
    return dot(rgb, vec3(0.2126, 0.7152, 0.0722));
}

float GE_KarisWeight(vec3 rgb, float lumaScale)
{
    return 1.0 / (1.0 + GE_KarisLuma(rgb) * lumaScale);
}

#endif // GE_KARIS_WEIGHT_GLSL
