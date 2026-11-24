cmake_minimum_required(VERSION 3.20)
project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(OB_NATIVE "Tune for the build machine (-march=native)" ON)
option(OB_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

add_library(orderbook INTERFACE)
target_include_directories(orderbook INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(orderbook INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
if(OB_NATIVE AND NOT OB_SANITIZE)
  target_compile_options(orderbook INTERFACE -march=native)
endif()
if(OB_SANITIZE)
  target_compile_options(orderbook INTERFACE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  target_link_options(orderbook INTERFACE -fsanitize=address,undefined)
endif()

add_executable(ob_cli src/ob_cli.cpp)
target_link_libraries(ob_cli PRIVATE orderbook)

add_executable(ob_bench bench/bench.cpp)
target_link_libraries(ob_bench PRIVATE orderbook)

add_executable(ob_unit_tests tests/unit_tests.cpp)
target_link_libraries(ob_unit_tests PRIVATE orderbook)

add_executable(ob_differential_test tests/differential_test.cpp)
target_link_libraries(ob_differential_test PRIVATE orderbook)

enable_testing()
add_test(NAME unit COMMAND ob_unit_tests)
add_test(NAME differential COMMAND ob_differential_test 300 4000)
