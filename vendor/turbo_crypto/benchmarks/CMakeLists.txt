cmake_add_benchmark(
  SOURCES benchmark_turbo_crypto.c
  LIBS turbo_crypto Rocida::TinyTest
  FOLDER "vendor/turbo_crypto/benchmarks")
