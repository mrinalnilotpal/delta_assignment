cmake_minimum_required(VERSION 3.20)
project(oms LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Warnings-as-errors for our own code only (kept off third-party targets like
# Catch2). NOTE: -Wpedantic is intentionally omitted because the design uses
# __int128, a documented compiler extension, for the fill-notional accumulator.
set(OMS_WARNINGS -Wall -Wextra -Werror)

# ---- Core library (domain types, order, pool, ...) --------------------------
file(GLOB OMS_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/*.cpp")
add_library(oms ${OMS_SOURCES})
target_include_directories(oms PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_compile_options(oms PRIVATE ${OMS_WARNINGS})

# ---- Simulated exchange + scenario driver -----------------------------------
add_subdirectory(sim)

# ---- Tests (Catch2 via FetchContent) ----------------------------------------
include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.4
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

enable_testing()
add_subdirectory(tests)
