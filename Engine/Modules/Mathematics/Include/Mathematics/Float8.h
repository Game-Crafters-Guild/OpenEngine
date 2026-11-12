#pragma once

#if defined(__AVX2__)
#include "Mathematics/Avx2Float8.h"
#else
#include "Mathematics/ScalarFloat8.h"
#endif
#include "Mathematics/CallingConvention.h"

namespace GameEngine::Mathematics
{

/// Eight float lanes for row code: Float8Detail::Avx2Float8 where the file compiles with AVX2 (every
/// x64 engine target), Float8Detail::ScalarFloat8 elsewhere. Both give the bits of the same scalar
/// code in each lane, so a row form with the operations of its scalar form, in the same order, has its bits.
/// Declare each helper that takes or returns Float8 with GE_VECTORCALL. The compiler can keep a
/// helper out of line, and the default Windows x64 convention then returns Float8 through memory.
#if defined(__AVX2__)
using Float8 = Float8Detail::Avx2Float8;
#else
using Float8 = Float8Detail::ScalarFloat8;
#endif

} // namespace GameEngine::Mathematics
