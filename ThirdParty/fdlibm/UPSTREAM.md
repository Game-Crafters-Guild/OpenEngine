# fdlibm (FreeBSD msun)

- Source: Sun's fdlibm 5.3 as kept in FreeBSD's `lib/msun/src`,
  https://github.com/freebsd/freebsd-src
- Upstream revision: `6a99232c75f6460b03a95019e2625828a158d457` (repository `main`;
  fetched 2026-11-03)
- License: the Sun Microsystems permission notice: "Permission to use, copy, modify,
  and distribute this software is freely granted, provided that this notice is
  preserved." `LICENSE.txt` holds the notice of each file below, copied byte for byte
  (trailing spaces included) from that revision.
- Adapted components, all in `Engine/Modules/Mathematics/Include/Mathematics/Transcendental.h`:
  - `KernelSin` is `__kernel_sin` (`k_sin.c`) and `KernelCos` is `__kernel_cos`
    (`k_cos.c`, FreeBSD's form): same expressions and coefficients.
  - `ReduceMedium` is the medium path of `__ieee754_rem_pio2` (`e_rem_pio2.c`): the
    three-part pi/2 constants, the rounds that run on cancellation, the same tests.
  - `Atan` is `atan` (`s_atan.c`): the same thresholds, the `atanhi`/`atanlo` tables and
    the 11 coefficients. `Atan2` takes the quotient form and the `pi - (z - pi_lo)` step of
    `__ieee754_atan2` (`e_atan2.c`) and the C rules for zeros and infinities.
  - `Log2` is the reduction and the `Lg1`-`Lg7` polynomial of `__ieee754_log`
    (`e_log.c`), scaled by 1/ln2 at the end. `Exp2` is the rational form of `e^r` and the
    `P1`-`P5` coefficients of `__ieee754_exp` (`e_exp.c`), with `2^t = 2^k * e^((t - k) * ln2)`.
  - `kTwoOverPiBits` holds the bits of 2/pi of the `ipio2` table in `k_rem_pio2.c`, in
    32-bit words.
- Not from upstream: `ReduceLarge` (the Payne-Hanek reduction of a float above
  2^20 * pi/2), the float entry points and their special cases, and `Pow` around `Log2`
  and `Exp2`. The engine code uses only plain IEEE double `+ - * /` and exact
  operations, so the results are the same on every platform.
