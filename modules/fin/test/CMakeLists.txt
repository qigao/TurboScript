set(TEST_NAME test_fin)

add_executable(${TEST_NAME} test_fin.c)

target_link_libraries(${TEST_NAME} PRIVATE fin TurboUtils::TinyTest TurboUtils::Core)

target_include_directories(${TEST_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

set_target_properties(
  ${TEST_NAME} PROPERTIES FOLDER modules/fin
                          RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})

# Factor processing tests
set(TEST_FACTOR_NAME test_factor)

add_executable(${TEST_FACTOR_NAME} test_factor.c)

target_link_libraries(${TEST_FACTOR_NAME} PRIVATE fin TurboUtils::TinyTest TurboUtils::Core)

target_include_directories(${TEST_FACTOR_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

set_target_properties(
  ${TEST_FACTOR_NAME} PROPERTIES FOLDER modules/fin
                                 RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME ${TEST_FACTOR_NAME} COMMAND ${TEST_FACTOR_NAME})

# Portfolio optimization tests
set(TEST_PORTFOLIO_NAME test_portfolio)

add_executable(${TEST_PORTFOLIO_NAME} test_portfolio.c)

target_link_libraries(${TEST_PORTFOLIO_NAME} PRIVATE fin TurboUtils::TinyTest TurboUtils::Core)

target_include_directories(${TEST_PORTFOLIO_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

set_target_properties(
  ${TEST_PORTFOLIO_NAME} PROPERTIES FOLDER modules/fin
                                    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME ${TEST_PORTFOLIO_NAME} COMMAND ${TEST_PORTFOLIO_NAME})

# Backtest and risk management tests
set(TEST_BACKTEST_RISK_NAME test_backtest_risk)

add_executable(${TEST_BACKTEST_RISK_NAME} test_backtest_risk.c)

target_link_libraries(${TEST_BACKTEST_RISK_NAME} PRIVATE fin TurboUtils::TinyTest TurboUtils::Core)

target_include_directories(${TEST_BACKTEST_RISK_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

set_target_properties(
  ${TEST_BACKTEST_RISK_NAME} PROPERTIES FOLDER modules/fin
                                        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME ${TEST_BACKTEST_RISK_NAME} COMMAND ${TEST_BACKTEST_RISK_NAME})

# Graph algorithms tests
set(TEST_GRAPH_NAME test_graph)

add_executable(${TEST_GRAPH_NAME} test_graph.c)

target_link_libraries(${TEST_GRAPH_NAME} PRIVATE fin TurboUtils::TinyTest TurboUtils::Core)

target_include_directories(${TEST_GRAPH_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

set_target_properties(
  ${TEST_GRAPH_NAME} PROPERTIES FOLDER modules/fin
                                RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME ${TEST_GRAPH_NAME} COMMAND ${TEST_GRAPH_NAME})

# Nash replicator tests
set(TEST_NASH_NAME test_nash)

add_executable(${TEST_NASH_NAME} test_nash.c)

target_link_libraries(${TEST_NASH_NAME} PRIVATE fin TurboUtils::TinyTest TurboUtils::Core)

target_include_directories(${TEST_NASH_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

set_target_properties(
  ${TEST_NASH_NAME} PROPERTIES FOLDER modules/fin
                                 RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME ${TEST_NASH_NAME} COMMAND ${TEST_NASH_NAME})

# Universe management tests
set(TEST_UNIVERSE_NAME test_universe)

add_executable(${TEST_UNIVERSE_NAME} test_universe.c)

target_link_libraries(${TEST_UNIVERSE_NAME} PRIVATE fin TurboUtils::TinyTest TurboUtils::Core)

target_include_directories(${TEST_UNIVERSE_NAME}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

set_target_properties(
  ${TEST_UNIVERSE_NAME} PROPERTIES FOLDER modules/fin
                                    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME ${TEST_UNIVERSE_NAME} COMMAND ${TEST_UNIVERSE_NAME})
