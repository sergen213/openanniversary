cmake_minimum_required(VERSION 3.28)
project(OpenAnniversary VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(OA_BUILD_TESTS "Build unit tests" ON)
option(OA_SANITIZE "Build with ASan+UBSan" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # The Vulkan structures are filled in by assigning fields after construction, so the aggregate-init
  # warning would be pure noise here.
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-missing-field-initializers)
  if(OA_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

find_package(ZLIB REQUIRED)
find_package(SDL3 REQUIRED CONFIG)
find_package(Vulkan REQUIRED)

# Shaders compile to SPIR-V at build time and load from <executable dir>/shaders at run time, so the repository
# stores GLSL rather than a binary blob and every shader shows up in the build log.
find_program(OA_GLSLANG glslangValidator REQUIRED)
set(OA_SHADER_OUTPUTS "")
foreach(source IN ITEMS shaders/quad.vert shaders/quad.frag shaders/mesh.vert shaders/mesh.frag)
  get_filename_component(shaderName "${source}" NAME)
  set(shaderOutput "${CMAKE_BINARY_DIR}/shaders/${shaderName}.spv")
  add_custom_command(OUTPUT "${shaderOutput}"
    COMMAND ${OA_GLSLANG} -V "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -o "${shaderOutput}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
    COMMENT "SPIR-V ${shaderName}")
  list(APPEND OA_SHADER_OUTPUTS "${shaderOutput}")
endforeach()
add_custom_target(oa_shaders ALL DEPENDS ${OA_SHADER_OUTPUTS})

# cdc container layer: bigfile archives of the Legend-era engine (docs/formats/CONTAINERS.md).
add_library(oa_cdc STATIC src/cdc/Bigfile.cpp src/cdc/Drm.cpp src/cdc/Texture.cpp src/cdc/Geometry.cpp)
target_include_directories(oa_cdc PUBLIC src)
target_link_libraries(oa_cdc PUBLIC ZLIB::ZLIB)

# Startup plumbing shared by every target: logging, configuration.
add_library(oa_core STATIC src/core/Log.cpp src/core/Config.cpp)
target_include_directories(oa_core PUBLIC src)
add_executable(oa-asset tools/oa-asset/main.cpp)
target_link_libraries(oa-asset PRIVATE oa_cdc oa_core)

# Native window/input layer (SDL3) and the Vulkan renderer.
add_library(oa_platform STATIC src/platform/Window.cpp)
target_include_directories(oa_platform PUBLIC src)
target_link_libraries(oa_platform PUBLIC oa_core SDL3::SDL3)

add_library(oa_render STATIC src/render/vk/VkRenderer.cpp)
target_include_directories(oa_render PUBLIC src)
target_link_libraries(oa_render PUBLIC oa_platform Vulkan::Vulkan)

add_executable(oa-game apps/game/main.cpp)
target_link_libraries(oa-game PRIVATE oa_render oa_cdc)
add_dependencies(oa-game oa_shaders)

if(OA_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(oa_tests tests/test_bigfile.cpp tests/test_core.cpp tests/test_drm.cpp tests/test_geometry.cpp
                       tests/test_texture.cpp)
  target_link_libraries(oa_tests PRIVATE oa_cdc oa_core GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(oa_tests)
endif()
