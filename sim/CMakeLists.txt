# Simulated exchange library (shared by the driver and the test suite).
add_library(oms_sim_lib SimulatedExchange.cpp)
target_include_directories(oms_sim_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(oms_sim_lib PUBLIC oms)
target_compile_options(oms_sim_lib PRIVATE ${OMS_WARNINGS})

# Scenario driver.
add_executable(oms_sim main.cpp)
target_link_libraries(oms_sim PRIVATE oms_sim_lib)
target_compile_options(oms_sim PRIVATE ${OMS_WARNINGS})

# Benchmark / results generator.
add_executable(oms_bench bench.cpp)
target_link_libraries(oms_bench PRIVATE oms_sim_lib)
target_compile_options(oms_bench PRIVATE ${OMS_WARNINGS})

# Signal-pipeline demo (producer -> gate -> netting for strategies 1..50).
add_executable(oms_signal_demo signal_demo.cpp)
target_link_libraries(oms_signal_demo PRIVATE oms)
target_compile_options(oms_signal_demo PRIVATE ${OMS_WARNINGS})
