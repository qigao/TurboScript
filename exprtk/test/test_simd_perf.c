/**
 * @file test_simd_perf.c
 * @brief Performance benchmarks for SIMD optimizations
 */

#include "simd_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

#define BENCH_SIZE 1000000
#define BENCH_ITERATIONS 100

static double* generate_random_data(size_t n) {
    double *data = malloc(n * sizeof(double));
    for (size_t i = 0; i < n; i++) {
        data[i] = (double)rand() / RAND_MAX * 100.0;
    }
    return data;
}

// Global sink to prevent compiler from optimizing away benchmarks
static volatile double g_sink = 0.0;

static double benchmark_ms(clock_t start, clock_t end) {
    return (double)(end - start) / CLOCKS_PER_SEC * 1000.0;
}

void bench_sum() {
    printf("\n=== Sum Benchmark ===\n");
    double *data = generate_random_data(BENCH_SIZE);

    // Scalar version
    clock_t start = clock();
    for (int iter = 0; iter < BENCH_ITERATIONS; iter++) {
        double sum = 0.0;
        for (size_t i = 0; i < BENCH_SIZE; i++) {
            sum += data[i];
        }
        g_sink += sum;
    }
    clock_t end = clock();
    double scalar_time = benchmark_ms(start, end);

    // SIMD version
    start = clock();
    for (int iter = 0; iter < BENCH_ITERATIONS; iter++) {
        double sum = simd_sum_avx(data, BENCH_SIZE);
        g_sink += sum;
    }
    end = clock();
    double simd_time = benchmark_ms(start, end);

    printf("  Data size: %d elements\n", BENCH_SIZE);
    printf("  Iterations: %d\n", BENCH_ITERATIONS);
    printf("  Scalar: %.2f ms\n", scalar_time);
    printf("  SIMD:   %.2f ms\n", simd_time);
    printf("  Speedup: %.2fx\n", scalar_time / simd_time);

    free(data);
}

void bench_dot_product() {
    printf("\n=== Dot Product Benchmark ===\n");
    double *a = generate_random_data(BENCH_SIZE);
    double *b = generate_random_data(BENCH_SIZE);

    // Scalar version
    clock_t start = clock();
    for (int iter = 0; iter < BENCH_ITERATIONS; iter++) {
        double sum = 0.0;
        for (size_t i = 0; i < BENCH_SIZE; i++) {
            sum += a[i] * b[i];
        }
        g_sink += sum;
    }
    clock_t end = clock();
    double scalar_time = benchmark_ms(start, end);

    // SIMD version
    start = clock();
    for (int iter = 0; iter < BENCH_ITERATIONS; iter++) {
        double sum = simd_dot_avx(a, b, BENCH_SIZE);
        g_sink += sum;
    }
    end = clock();
    double simd_time = benchmark_ms(start, end);

    printf("  Data size: %d elements\n", BENCH_SIZE);
    printf("  Iterations: %d\n", BENCH_ITERATIONS);
    printf("  Scalar: %.2f ms\n", scalar_time);
    printf("  SIMD:   %.2f ms\n", simd_time);
    printf("  Speedup: %.2fx\n", scalar_time / simd_time);

    free(a);
    free(b);
}

void bench_zscore() {
    printf("\n=== Z-Score Benchmark ===\n");
    double *data = generate_random_data(BENCH_SIZE);
    double *out = malloc(BENCH_SIZE * sizeof(double));

    // Compute mean and sd
    double sum = 0.0;
    for (size_t i = 0; i < BENCH_SIZE; i++) {
        sum += data[i];
    }
    double mean = sum / BENCH_SIZE;

    double m2 = 0.0;
    for (size_t i = 0; i < BENCH_SIZE; i++) {
        double d = data[i] - mean;
        m2 += d * d;
    }
    double sd = sqrt(m2 / (BENCH_SIZE - 1));

    // Scalar version
    clock_t start = clock();
    for (int iter = 0; iter < BENCH_ITERATIONS; iter++) {
        for (size_t i = 0; i < BENCH_SIZE; i++) {
            out[i] = (data[i] - mean) / sd;
        }
    }
    clock_t end = clock();
    double scalar_time = benchmark_ms(start, end);

    // SIMD version
    start = clock();
    for (int iter = 0; iter < BENCH_ITERATIONS; iter++) {
        simd_zscore_avx(data, out, BENCH_SIZE, mean, sd);
    }
    end = clock();
    double simd_time = benchmark_ms(start, end);

    printf("  Data size: %d elements\n", BENCH_SIZE);
    printf("  Iterations: %d\n", BENCH_ITERATIONS);
    printf("  Scalar: %.2f ms\n", scalar_time);
    printf("  SIMD:   %.2f ms\n", simd_time);
    printf("  Speedup: %.2fx\n", scalar_time / simd_time);

    free(data);
    free(out);
}

void bench_mean_variance() {
    printf("\n=== Mean & Variance Benchmark ===\n");
    double *data = generate_random_data(BENCH_SIZE);

    // Scalar version (two-pass)
    clock_t start = clock();
    for (int iter = 0; iter < BENCH_ITERATIONS; iter++) {
        double sum = 0.0;
        for (size_t i = 0; i < BENCH_SIZE; i++) {
            sum += data[i];
        }
        double mean = sum / BENCH_SIZE;

        double m2 = 0.0;
        for (size_t i = 0; i < BENCH_SIZE; i++) {
            double d = data[i] - mean;
            m2 += d * d;
        }
        double variance = m2 / (BENCH_SIZE - 1);
        g_sink += mean + variance;
    }
    clock_t end = clock();
    double scalar_time = benchmark_ms(start, end);

    // SIMD version (Welford + SIMD)
    start = clock();
    for (int iter = 0; iter < BENCH_ITERATIONS; iter++) {
        double mean, variance;
        simd_mean_variance_avx(data, BENCH_SIZE, &mean, &variance);
        g_sink += mean + variance;
    }
    end = clock();
    double simd_time = benchmark_ms(start, end);

    printf("  Data size: %d elements\n", BENCH_SIZE);
    printf("  Iterations: %d\n", BENCH_ITERATIONS);
    printf("  Scalar (2-pass): %.2f ms\n", scalar_time);
    printf("  SIMD (Welford):  %.2f ms\n", simd_time);
    printf("  Speedup: %.2fx\n", scalar_time / simd_time);

    free(data);
}

void bench_scale() {
    printf("\n=== Scalar Multiplication Benchmark ===\n");
    double *data = generate_random_data(BENCH_SIZE);
    double *out = malloc(BENCH_SIZE * sizeof(double));
    double scalar = 3.14159;

    // Scalar version
    clock_t start = clock();
    for (int iter = 0; iter < BENCH_ITERATIONS; iter++) {
        for (size_t i = 0; i < BENCH_SIZE; i++) {
            out[i] = data[i] * scalar;
        }
    }
    clock_t end = clock();
    double scalar_time = benchmark_ms(start, end);

    // SIMD version
    start = clock();
    for (int iter = 0; iter < BENCH_ITERATIONS; iter++) {
        simd_scale_avx(data, out, scalar, BENCH_SIZE);
    }
    end = clock();
    double simd_time = benchmark_ms(start, end);

    printf("  Data size: %d elements\n", BENCH_SIZE);
    printf("  Iterations: %d\n", BENCH_ITERATIONS);
    printf("  Scalar: %.2f ms\n", scalar_time);
    printf("  SIMD:   %.2f ms\n", simd_time);
    printf("  Speedup: %.2fx\n", scalar_time / simd_time);

    free(data);
    free(out);
}

int main() {
    printf("=================================================\n");
    printf("SIMD Performance Benchmarks\n");
    printf("=================================================\n");

    // Check CPU features
    printf("\nCPU Features:\n");
    printf("  AVX:  %s\n", simd_has_avx() ? "YES" : "NO");
    printf("  AVX2: %s\n", simd_has_avx2() ? "YES" : "NO");

    if (!simd_has_avx()) {
        printf("\nWARNING: AVX not supported. SIMD functions will use scalar path.\n");
    }

    // Run benchmarks
    bench_sum();
    bench_dot_product();
    bench_zscore();
    bench_mean_variance();
    bench_scale();

    printf("\n=================================================\n");
    printf("Benchmarks completed!\n");
    printf("=================================================\n");

    // Use g_sink to prevent optimization
    if (g_sink == 0.0) {
        printf("(This should never print)\n");
    }

    return 0;
}
