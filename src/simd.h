#pragma once
#include <immintrin.h>

namespace mvdb {

__attribute__((target("avx2,fma")))
inline float l2_sq_avx2(const float* a, const float* b, int n) {
    __m256 s0 = _mm256_setzero_ps();
    __m256 s1 = _mm256_setzero_ps();
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
        __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8));
        s0 = _mm256_fmadd_ps(d0, d0, s0);
        s1 = _mm256_fmadd_ps(d1, d1, s1);
    }
    for (; i + 8 <= n; i += 8) {
        __m256 d = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
        s0 = _mm256_fmadd_ps(d, d, s0);
    }
    __m256 sum = _mm256_add_ps(s0, s1);
    __m128 hi = _mm256_extractf128_ps(sum, 1);
    __m128 lo = _mm256_castps256_ps128(sum);
    __m128 s = _mm_add_ps(lo, hi);
    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);
    float r = _mm_cvtss_f32(s);
    for (; i < n; i++) { float d = a[i] - b[i]; r += d * d; }
    return r;
}

inline bool cpu_has_avx2_fma() {
    static int v = -1;
    if (v < 0) v = (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) ? 1 : 0;
    return v == 1;
}

inline float l2_sq_fast(const float* a, const float* b, int n) {
    if (cpu_has_avx2_fma()) return l2_sq_avx2(a, b, n);
    float sum = 0;
    for (int i = 0; i < n; i++) { float d = a[i] - b[i]; sum += d * d; }
    return sum;
}

}  // namespace mvdb