#pragma once
#include <immintrin.h>

namespace mvdb {

__attribute__((target("avx512f")))
inline float l2_sq_avx512(const float* a, const float* b, int n) {
    __m512 s0 = _mm512_setzero_ps();
    __m512 s1 = _mm512_setzero_ps();
    __m512 s2 = _mm512_setzero_ps();
    __m512 s3 = _mm512_setzero_ps();
    int i = 0;
    // 64-dim chunks with 4 independent accumulator chains hide FMA latency
    for (; i + 64 <= n; i += 64) {
        __m512 d0 = _mm512_sub_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i));
        __m512 d1 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 16), _mm512_loadu_ps(b + i + 16));
        __m512 d2 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 32), _mm512_loadu_ps(b + i + 32));
        __m512 d3 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 48), _mm512_loadu_ps(b + i + 48));
        s0 = _mm512_fmadd_ps(d0, d0, s0);
        s1 = _mm512_fmadd_ps(d1, d1, s1);
        s2 = _mm512_fmadd_ps(d2, d2, s2);
        s3 = _mm512_fmadd_ps(d3, d3, s3);
    }
    for (; i + 32 <= n; i += 32) {
        __m512 d0 = _mm512_sub_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i));
        __m512 d1 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 16), _mm512_loadu_ps(b + i + 16));
        s0 = _mm512_fmadd_ps(d0, d0, s0);
        s1 = _mm512_fmadd_ps(d1, d1, s1);
    }
    for (; i + 16 <= n; i += 16) {
        __m512 d = _mm512_sub_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i));
        s0 = _mm512_fmadd_ps(d, d, s0);
    }
    __m512 sum = _mm512_add_ps(_mm512_add_ps(s0, s1), _mm512_add_ps(s2, s3));
    float r = _mm512_reduce_add_ps(sum);
    for (; i < n; i++) { float d = a[i] - b[i]; r += d * d; }
    return r;
}

__attribute__((target("avx2,fma")))
inline float l2_sq_avx2(const float* a, const float* b, int n) {
    __m256 s0 = _mm256_setzero_ps();
    __m256 s1 = _mm256_setzero_ps();
    __m256 s2 = _mm256_setzero_ps();
    __m256 s3 = _mm256_setzero_ps();
    int i = 0;
    for (; i + 32 <= n; i += 32) {
        __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
        __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8));
        __m256 d2 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16));
        __m256 d3 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24));
        s0 = _mm256_fmadd_ps(d0, d0, s0);
        s1 = _mm256_fmadd_ps(d1, d1, s1);
        s2 = _mm256_fmadd_ps(d2, d2, s2);
        s3 = _mm256_fmadd_ps(d3, d3, s3);
    }
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
    __m256 sum = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
    __m128 hi = _mm256_extractf128_ps(sum, 1);
    __m128 lo = _mm256_castps256_ps128(sum);
    __m128 s = _mm_add_ps(lo, hi);
    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);
    float r = _mm_cvtss_f32(s);
    for (; i < n; i++) { float d = a[i] - b[i]; r += d * d; }
    return r;
}

inline int cpu_simd_level() {
    static int v = -1;
    if (v < 0) {
        if (__builtin_cpu_supports("avx512f")) v = 3;
        else if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) v = 2;
        else v = 1;
    }
    return v;
}

inline float l2_sq_fast(const float* a, const float* b, int n) {
    int lvl = cpu_simd_level();
    if (lvl >= 3) return l2_sq_avx512(a, b, n);
    if (lvl >= 2) return l2_sq_avx2(a, b, n);
    float sum = 0;
    for (int i = 0; i < n; i++) { float d = a[i] - b[i]; sum += d * d; }
    return sum;
}

}  // namespace mvdb