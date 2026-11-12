#pragma once

/// `Float8 GE_VECTORCALL F(Float8 x)`: on MSVC x64 the macro is `__vectorcall`, which passes and returns a
/// type with one `__m256` member in a ymm register, not through memory. Elsewhere it is empty: the System V
/// x64 convention already uses a ymm register, and ARM64 has no `__vectorcall`.

#if defined(_MSC_VER) && defined(_M_X64) && !defined(_M_ARM64EC)
#define GE_VECTORCALL __vectorcall
#else
#define GE_VECTORCALL
#endif
