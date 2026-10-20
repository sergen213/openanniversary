cmake_minimum_required(VERSION 3.28)
project(OpenAnniversary VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OA_BUILD_TESTS "Build unit tests" ON)
option(OA_SANITIZE "Build with ASan+UBSan" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
  if(OA_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

find_package(ZLIB REQUIRED)

# cdc container layer: bigfile archives of the Legend-era engine (docs/formats/CONTAINERS.md).
add_library(oa_cdc STATIC src/cdc/Bigfile.cpp)
target_include_directories(oa_cdc PUBLIC src)
target_link_libraries(oa_cdc PUBLIC ZLIB::ZLIB)

add_executable(oa-asset tools/oa-asset/main.cpp)
target_link_libraries(oa-asset PRIVATE oa_cdc)

if(OA_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(oa_tests tests/test_bigfile.cpp)
  target_link_libraries(oa_tests PRIVATE oa_cdc GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(oa_tests)
endif()
