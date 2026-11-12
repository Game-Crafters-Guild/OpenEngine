# Noise module: the engine's shared CPU noise primitives.
#
# Header-only, so the sampling loops that call it keep inlining them. The module
# header lists what lives here and what deliberately stays with its own GPU
# mirror; see Include/Noise/Gradient/GradientNoise2D.h.

cmake_minimum_required(VERSION 3.20)

project(Noise LANGUAGES CXX)

add_library(Noise INTERFACE)

# Public includes: exposes headers under "Noise/..." to dependents.
target_include_directories(Noise INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Include)

# Types supplies the integer types and the lattice hash; Mathematics the vector and position types of the 3D fields.
target_link_libraries(Noise INTERFACE Types Mathematics)

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMakeLists.txt")
    add_subdirectory(Tests)
endif()
