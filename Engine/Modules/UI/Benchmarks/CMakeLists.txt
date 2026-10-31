find_package(benchmark CONFIG REQUIRED)

add_executable(UILayoutBenchmark EXCLUDE_FROM_ALL ElementBuilderBenchmark.cpp)
target_compile_features(UILayoutBenchmark PRIVATE cxx_std_20)
target_link_libraries(UILayoutBenchmark PRIVATE Engine benchmark::benchmark)
target_compile_definitions(UILayoutBenchmark PRIVATE
    $<$<CONFIG:Release>:GE_LAYOUT_PROFILE_RELEASE=1>
    GE_LAYOUT_PROFILE_CONFIG="$<CONFIG>"
)
set_target_properties(UILayoutBenchmark PROPERTIES FOLDER "UI/Benchmarks")
ge_set_output_benchmarks(UILayoutBenchmark)
ge_stage_runtime_dependencies(UILayoutBenchmark)
