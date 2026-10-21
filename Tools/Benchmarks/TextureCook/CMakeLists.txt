# TextureCookBenchmark: wall time of the texture cook's encode of one image's mip chain on N encode
# threads, and the level and payload dump its quality scoring reads. A diagnostic, not a test; its
# numbers mean something only from a Release build on a quiet machine. EXCLUDE_FROM_ALL keeps it out
# of every default build:
#   cmake --build --preset vs2026-x64-local --config Release --target TextureCookBenchmark
add_executable(TextureCookBenchmark EXCLUDE_FROM_ALL TextureCookBenchmark.cpp)
target_compile_features(TextureCookBenchmark PRIVATE cxx_std_20)
target_link_libraries(TextureCookBenchmark PRIVATE Engine)
ge_set_output_benchmarks(TextureCookBenchmark)
ge_stage_runtime_dependencies(TextureCookBenchmark)
