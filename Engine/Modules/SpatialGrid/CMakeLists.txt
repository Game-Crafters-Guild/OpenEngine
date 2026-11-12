cmake_minimum_required(VERSION 3.20)

project(SpatialGrid LANGUAGES CXX)

# The gameplay spatial grid of moving spheres and its queries. No World, no ECS.
add_library(SpatialGrid OBJECT
    Source/Grid.cpp
)

target_compile_features(SpatialGrid PUBLIC cxx_std_20)

target_include_directories(SpatialGrid
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
)

target_link_libraries(SpatialGrid
    PUBLIC
        Types
        Mathematics
    PRIVATE
        Memory
)
