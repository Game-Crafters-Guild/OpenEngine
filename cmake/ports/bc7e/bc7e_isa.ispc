// The instruction set bc7e runs on. Compiled with the same target list as
// bc7e.ispc, so on x64 its dispatcher makes the same choice as bc7e's.
export uniform int bc7e_isa()
{
#if defined(ISPC_TARGET_AVX512SKX)
    return 3;
#elif defined(ISPC_TARGET_AVX2)
    return 2;
#elif defined(ISPC_TARGET_SSE4)
    return 1;
#elif defined(ISPC_TARGET_NEON)
    return 4;
#else
    return 0;
#endif
}
