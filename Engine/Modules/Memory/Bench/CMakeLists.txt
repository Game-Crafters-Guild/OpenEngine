# MemoryBench: the Memory module's micro-benches, run in Release; not part of the default
# build.
add_executable(MemoryBench EXCLUDE_FROM_ALL MemoryBench.cpp)
target_compile_features(MemoryBench PRIVATE cxx_std_20)
target_link_libraries(MemoryBench PRIVATE Memory)
