#ifndef GE_INTEGER_HASH_GLSL
#define GE_INTEGER_HASH_GLSL

// A 32-bit integer hash (the PCG RXS-M-XS permutation, O'Neill 2014): every input bit reaches every
// output bit, so consecutive keys give unrelated values. Exact on every device: integer arithmetic only.
uint GE_HashU32(uint key)
{
    uint state = key * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// A uniform value in [0, 1) from a key: the hash's top 24 bits, which a float holds exactly.
float GE_HashUnit(uint key)
{
    return float(GE_HashU32(key) >> 8u) / 16777216.0;
}

#endif // GE_INTEGER_HASH_GLSL
