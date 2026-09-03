cmake_add_test(
  SOURCES test_turbo_crypto.c
  LIBS turbo_crypto Salts::TinyTest
  FOLDER "vendor/turbo_crypto/tests")
