// Conversions between the engine's natural units (float: Hz, ms, 0..1) and the library's types, and
// the few sample operations the rack's own modules need, written once for float, Q15 and Q31 with
// the library's helpers.
#pragma once

#include "pcl/pcl.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rack
{

// ---- control side: natural units -> the library's types (these may use floating point; they are
// not on the sample path)
inline pcl_hz_t      to_hz(double v) { return PCL_HZ(v); }
inline pcl_coef_t    to_coef(double v) { return PCL_COEF(v); }
inline pcl_oct_t     to_oct(double v) { return PCL_OCT(v); }
inline pcl_db_t      to_db(double v) { return PCL_DB(v); }
inline pcl_quality_t to_quality(double v) { return PCL_QUALITY(v); }
inline uint32_t      to_u32(float v) { return v <= 0.f ? 0u : (uint32_t)std::lround(v); }
inline int           to_int(float v) { return (int)std::lround(v); }

// A value in [-1, 1] as a sample.
#if PCL_SAMPLE_TYPE == PCL_FLOAT
inline pcl_sample_t           to_sample(double v) { return (pcl_sample_t)v; }
inline constexpr pcl_sample_t kFullScale = 1.0f; // +1.0: the level of a gate
using wide_acc_t = float;
inline pcl_sample_t sat_acc(wide_acc_t a) { return a; }
inline pcl_sample_t mul_coef(pcl_sample_t x, pcl_coef_t c) { return x * c; }
inline float        sample_to_float(pcl_sample_t s) { return s; }
#elif PCL_SAMPLE_TYPE == PCL_Q15
inline pcl_sample_t           to_sample(double v) { return PCL_CONST_Q15(v); }
inline constexpr pcl_sample_t kFullScale = INT16_MAX;
using wide_acc_t = int32_t; // a few saturated Q15 values cannot overflow it
inline pcl_sample_t sat_acc(wide_acc_t a) { return pcl_sat_q15(a); }
inline pcl_sample_t mul_coef(pcl_sample_t x, pcl_coef_t c) { return pcl_mul_q15_q30(x, c); }
inline float        sample_to_float(pcl_sample_t s) { return (float)s / 32768.0f; }
#else
inline pcl_sample_t           to_sample(double v) { return PCL_CONST_Q31(v); }
inline constexpr pcl_sample_t kFullScale = INT32_MAX;
using wide_acc_t = int64_t;
inline pcl_sample_t sat_acc(wide_acc_t a) { return pcl_sat_q31(a); }
inline pcl_sample_t mul_coef(pcl_sample_t x, pcl_coef_t c) { return pcl_mul_q31_q30(x, c); }
inline float        sample_to_float(pcl_sample_t s) { return (float)((double)s / 2147483648.0); }
#endif

// A frequency below 0.45 of the sample rate, as every library module requires.
inline double clamp_hz(double hz, uint32_t sample_rate)
{
    return std::min(hz, 0.45 * (double)sample_rate - 1.0);
}

} // namespace rack
