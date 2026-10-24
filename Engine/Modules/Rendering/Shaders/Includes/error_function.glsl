#ifndef GE_ERROR_FUNCTION_GLSL
#define GE_ERROR_FUNCTION_GLSL

// The error function erf(x), within about 5e-7 in 32-bit floats (Abramowitz and Stegun 7.1.26). It
// reaches -1 and 1 exactly, so its differences over adjacent intervals add up to the exact total.
float GE_Erf(float x)
{
    float s = sign(x);
    float a = abs(x);
    float t = 1.0 / (1.0 + 0.3275911 * a);
    float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t
                      - 0.284496736) * t + 0.254829592) * t * exp(-a * a);
    return s * y;
}

#endif // GE_ERROR_FUNCTION_GLSL
