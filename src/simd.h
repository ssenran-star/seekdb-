#pragma once
#include <immintrin.h>

namespace mvdb {

__attribute__((target("avx512f")))
inline float l2_sq_avx512(const float* a, const float* b, int n) {
    __m512 s0 = _mm512_setzero_ps();
    __m512 s1 = _mm512_setzero_ps();
    int i = 0;
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
    __m512 sum = _mm512_add_ps(s0, s1);
    float r = _mm512_reduce_add_ps(sum);
    for (; i < n; i++) { float d = a[i] - b[i]; r += d * d; }
    return r;
}

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

inline int cpu_simd_level() {
    static int v = -1;
    if (v < 0) {
        if (__builtin_cpu_supports("avx512f")) v = 3;
        else if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) v = 2;
        else v = 1;
    }
    return v;
}

inline float l2_sq_scalar(const float* a, const float* b, int n) {
    float sum = 0;
    for (int i = 0; i < n; i++) { float d = a[i] - b[i]; sum += d * d; }
    return sum;
}

inline float hsum256(__m256 v) {
    __m128 hi = _mm256_extractf128_ps(v, 1);
    __m128 lo = _mm256_castps256_ps128(v);
    __m128 s = _mm_add_ps(lo, hi);
    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);
    return _mm_cvtss_f32(s);
}

__attribute__((target("avx2,fma")))
inline float l2_sq_avx2_abort(const float* a, const float* b, int n, float thresh) {
    __m256 s0 = _mm256_setzero_ps();
    __m256 s1 = _mm256_setzero_ps();
    int i = 0;
    for (; i + 16 <= n; i += 16) {
        __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
        __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8));
        s0 = _mm256_fmadd_ps(d0, d0, s0);
        s1 = _mm256_fmadd_ps(d1, d1, s1);
        if ((i + 16) == 64 || (i + 16) == 128) {
            float part = hsum256(_mm256_add_ps(s0, s1));
            if (part > thresh) return part;
        }
    }
    for (; i + 8 <= n; i += 8) {
        __m256 d = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
        s0 = _mm256_fmadd_ps(d, d, s0);
    }
    float r = hsum256(_mm256_add_ps(s0, s1));
    for (; i < n; i++) { float d = a[i] - b[i]; r += d * d; }
    return r;
}

__attribute__((target("avx512f")))
inline float l2_sq_avx512_abort(const float* a, const float* b, int n, float thresh) {
    __m512 s0 = _mm512_setzero_ps();
    __m512 s1 = _mm512_setzero_ps();
    int i = 0;
    for (; i + 32 <= n; i += 32) {
        __m512 d0 = _mm512_sub_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i));
        __m512 d1 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 16), _mm512_loadu_ps(b + i + 16));
        s0 = _mm512_fmadd_ps(d0, d0, s0);
        s1 = _mm512_fmadd_ps(d1, d1, s1);
        if ((i + 32) == 64 || (i + 32) == 128) {
            float part = _mm512_reduce_add_ps(_mm512_add_ps(s0, s1));
            if (part > thresh) return part;
        }
    }
    for (; i + 16 <= n; i += 16) {
        __m512 d = _mm512_sub_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i));
        s0 = _mm512_fmadd_ps(d, d, s0);
    }
    float r = _mm512_reduce_add_ps(_mm512_add_ps(s0, s1));
    for (; i < n; i++) { float d = a[i] - b[i]; r += d * d; }
    return r;
}

__attribute__((target("avx512f")))
inline float l2_sq_avx512_128(const float* a, const float* b) {
    __m512 s0 = _mm512_setzero_ps();
    __m512 s1 = _mm512_setzero_ps();
    for (int i = 0; i < 128; i += 32) {
        __m512 d0 = _mm512_sub_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i));
        __m512 d1 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 16), _mm512_loadu_ps(b + i + 16));
        s0 = _mm512_fmadd_ps(d0, d0, s0);
        s1 = _mm512_fmadd_ps(d1, d1, s1);
    }
    return _mm512_reduce_add_ps(_mm512_add_ps(s0, s1));
}

__attribute__((target("avx512f")))
inline float l2_sq_avx512_128_abort(const float* a, const float* b, float thresh) {
    __m512 s0 = _mm512_setzero_ps();
    __m512 s1 = _mm512_setzero_ps();
    for (int i = 0; i < 64; i += 32) {
        __m512 d0 = _mm512_sub_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i));
        __m512 d1 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 16), _mm512_loadu_ps(b + i + 16));
        s0 = _mm512_fmadd_ps(d0, d0, s0);
        s1 = _mm512_fmadd_ps(d1, d1, s1);
    }
    float part = _mm512_reduce_add_ps(_mm512_add_ps(s0, s1));
    if (part > thresh) return part;
    for (int i = 64; i < 128; i += 32) {
        __m512 d0 = _mm512_sub_ps(_mm512_loadu_ps(a + i), _mm512_loadu_ps(b + i));
        __m512 d1 = _mm512_sub_ps(_mm512_loadu_ps(a + i + 16), _mm512_loadu_ps(b + i + 16));
        s0 = _mm512_fmadd_ps(d0, d0, s0);
        s1 = _mm512_fmadd_ps(d1, d1, s1);
    }
    return _mm512_reduce_add_ps(_mm512_add_ps(s0, s1));
}

__attribute__((target("avx2,fma")))
inline float l2_sq_avx2_128(const float* a, const float* b) {
    __m256 s0 = _mm256_setzero_ps();
    __m256 s1 = _mm256_setzero_ps();
    for (int i = 0; i < 128; i += 16) {
        __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
        __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8));
        s0 = _mm256_fmadd_ps(d0, d0, s0);
        s1 = _mm256_fmadd_ps(d1, d1, s1);
    }
    return hsum256(_mm256_add_ps(s0, s1));
}

__attribute__((target("avx2,fma")))
inline float l2_sq_avx2_128_abort(const float* a, const float* b, float thresh) {
    __m256 s0 = _mm256_setzero_ps();
    __m256 s1 = _mm256_setzero_ps();
    for (int i = 0; i < 64; i += 16) {
        __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
        __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8));
        s0 = _mm256_fmadd_ps(d0, d0, s0);
        s1 = _mm256_fmadd_ps(d1, d1, s1);
    }
    float part = hsum256(_mm256_add_ps(s0, s1));
    if (part > thresh) return part;
    for (int i = 64; i < 128; i += 16) {
        __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
        __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8));
        s0 = _mm256_fmadd_ps(d0, d0, s0);
        s1 = _mm256_fmadd_ps(d1, d1, s1);
    }
    return hsum256(_mm256_add_ps(s0, s1));
}

__attribute__((target("avx512f")))
inline void l2_sq_avx512_128x4(const float* r0, const float* r1, const float* r2, const float* r3,
                               const float* q, float* out) {
    __m512 s0 = _mm512_setzero_ps();
    __m512 s1 = _mm512_setzero_ps();
    __m512 s2 = _mm512_setzero_ps();
    __m512 s3 = _mm512_setzero_ps();
    for (int i = 0; i < 128; i += 16) {
        __m512 qq = _mm512_loadu_ps(q + i);
        __m512 d0 = _mm512_sub_ps(_mm512_loadu_ps(r0 + i), qq);
        __m512 d1 = _mm512_sub_ps(_mm512_loadu_ps(r1 + i), qq);
        __m512 d2 = _mm512_sub_ps(_mm512_loadu_ps(r2 + i), qq);
        __m512 d3 = _mm512_sub_ps(_mm512_loadu_ps(r3 + i), qq);
        s0 = _mm512_fmadd_ps(d0, d0, s0);
        s1 = _mm512_fmadd_ps(d1, d1, s1);
        s2 = _mm512_fmadd_ps(d2, d2, s2);
        s3 = _mm512_fmadd_ps(d3, d3, s3);
    }
    out[0] = _mm512_reduce_add_ps(s0);
    out[1] = _mm512_reduce_add_ps(s1);
    out[2] = _mm512_reduce_add_ps(s2);
    out[3] = _mm512_reduce_add_ps(s3);
}

__attribute__((target("avx2,fma")))
inline void l2_sq_avx2_128x4(const float* r0, const float* r1, const float* r2, const float* r3,
                             const float* q, float* out) {
    __m256 s0 = _mm256_setzero_ps();
    __m256 s1 = _mm256_setzero_ps();
    __m256 s2 = _mm256_setzero_ps();
    __m256 s3 = _mm256_setzero_ps();
    __m256 t0 = _mm256_setzero_ps();
    __m256 t1 = _mm256_setzero_ps();
    __m256 t2 = _mm256_setzero_ps();
    __m256 t3 = _mm256_setzero_ps();
    for (int i = 0; i < 128; i += 16) {
        __m256 q0 = _mm256_loadu_ps(q + i);
        __m256 q1 = _mm256_loadu_ps(q + i + 8);
        __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(r0 + i), q0);
        __m256 e0 = _mm256_sub_ps(_mm256_loadu_ps(r0 + i + 8), q1);
        __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(r1 + i), q0);
        __m256 e1 = _mm256_sub_ps(_mm256_loadu_ps(r1 + i + 8), q1);
        __m256 d2 = _mm256_sub_ps(_mm256_loadu_ps(r2 + i), q0);
        __m256 e2 = _mm256_sub_ps(_mm256_loadu_ps(r2 + i + 8), q1);
        __m256 d3 = _mm256_sub_ps(_mm256_loadu_ps(r3 + i), q0);
        __m256 e3 = _mm256_sub_ps(_mm256_loadu_ps(r3 + i + 8), q1);
        s0 = _mm256_fmadd_ps(d0, d0, s0); t0 = _mm256_fmadd_ps(e0, e0, t0);
        s1 = _mm256_fmadd_ps(d1, d1, s1); t1 = _mm256_fmadd_ps(e1, e1, t1);
        s2 = _mm256_fmadd_ps(d2, d2, s2); t2 = _mm256_fmadd_ps(e2, e2, t2);
        s3 = _mm256_fmadd_ps(d3, d3, s3); t3 = _mm256_fmadd_ps(e3, e3, t3);
    }
    out[0] = hsum256(_mm256_add_ps(s0, t0));
    out[1] = hsum256(_mm256_add_ps(s1, t1));
    out[2] = hsum256(_mm256_add_ps(s2, t2));
    out[3] = hsum256(_mm256_add_ps(s3, t3));
}

inline void l2_sq_fast_128x4(const float* r0, const float* r1, const float* r2, const float* r3,
                             const float* q, float* out) {
    int lvl = cpu_simd_level();
    if (lvl >= 3) l2_sq_avx512_128x4(r0, r1, r2, r3, q, out);
    else if (lvl >= 2) l2_sq_avx2_128x4(r0, r1, r2, r3, q, out);
    else {
        out[0] = l2_sq_scalar(r0, q, 128);
        out[1] = l2_sq_scalar(r1, q, 128);
        out[2] = l2_sq_scalar(r2, q, 128);
        out[3] = l2_sq_scalar(r3, q, 128);
    }
}

inline float l2_sq_fast(const float* a, const float* b, int n) {
    int lvl = cpu_simd_level();
    if (n == 128) {
        if (lvl >= 3) return l2_sq_avx512_128(a, b);
        if (lvl >= 2) return l2_sq_avx2_128(a, b);
        return l2_sq_scalar(a, b, n);
    }
    if (lvl >= 3) return l2_sq_avx512(a, b, n);
    if (lvl >= 2) return l2_sq_avx2(a, b, n);
    return l2_sq_scalar(a, b, n);
}

inline float l2_sq_fast_abort(const float* a, const float* b, int n, float thresh) {
    int lvl = cpu_simd_level();
    if (n == 128) {
        if (lvl >= 3) return l2_sq_avx512_128_abort(a, b, thresh);
        if (lvl >= 2) return l2_sq_avx2_128_abort(a, b, thresh);
    }
    if (lvl >= 3) return l2_sq_avx512_abort(a, b, n, thresh);
    if (lvl >= 2) return l2_sq_avx2_abort(a, b, n, thresh);
    float sum = 0;
    for (int i = 0; i < n; i++) {
        float d = a[i] - b[i];
        sum += d * d;
        if ((i & 31) == 31 && sum > thresh) return sum;
    }
    return sum;
}

}  // namespace mvdb