include(Catch)

# Tests that need only the core library.
function(add_oms_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE oms Catch2::Catch2WithMain)
  target_compile_options(${name} PRIVATE ${OMS_WARNINGS})
  catch_discover_tests(${name})
endfunction()

# Tests that also need the simulated exchange.
function(add_sim_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE oms_sim_lib Catch2::Catch2WithMain)
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(${name} PRIVATE ${OMS_WARNINGS})
  catch_discover_tests(${name})
endfunction()

add_oms_test(TestOrderId)
add_oms_test(TestOrderPool)
add_oms_test(TestOrder)
add_oms_test(TestHealth)
add_oms_test(TestSignal)
add_oms_test(TestNetting)
add_oms_test(TestReconciliation)

add_sim_test(TestOrderManager)
add_sim_test(TestRouter)
add_sim_test(TestSimulatedExchange)
add_sim_test(TestExecutionAlgo)
add_sim_test(TestFailureModes)
add_sim_test(TestPeriodicRecon)
add_sim_test(TestMetrics)
