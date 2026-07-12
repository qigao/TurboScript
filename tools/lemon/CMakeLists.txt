add_executable(lemon lemon.c)
add_executable(TurboNet::Lemon ALIAS lemon)
cmake_config_target(lemon FOLDER tools)
target_compile_features(lemon PRIVATE c_std_11)
