# UI layout construction profiling

`UILayoutBenchmark` measures registered control creation and retained-tree construction
and destruction. It does not solve Yoga layout, resolve CSS, render, or initialize an
editor. It uses Google Benchmark and is excluded from default builds and CTest.

Configure with `BUILD_BENCHMARKS=ON`, then build the target:

```powershell
cmake --preset vs2026-x64-local -DBUILD_TESTING=ON -DBUILD_BENCHMARKS=ON
cmake --build --preset vs2026-x64-local --config Release --target UILayoutBenchmark -j 12
```

Use the repository's workbench build and timing-lock wrappers when available. Fresh
worktrees also need the shared toolchain and their own vcpkg root as described in
`AGENTS.md`. No new dependencies are needed beyond the existing test/benchmark feature.

The executable is staged at
`build/vs2026-x64-local/bin/Release/Benchmarks/UILayoutBenchmark.exe` with its runtime DLLs.
Run from that staged directory. Paths supplied to Windows programs must use drive-letter
paths, including the JSON output path.

## Verify the workloads

```powershell
./UILayoutBenchmark.exe --verify
```

Verification runs every population, factory case, and tree size without timings. It
checks factory results, registered defaults, child counts/order, values, classes, and
IDs. Both DebugFast and Release support this mode; timing runs reject other configurations.

## Cases

| Case | Work measured |
|---|---|
| `Factory/Inferred` | Typed creation with canonical-tag inference. |
| `Factory/Explicit` | Typed creation with an explicit canonical tag. |
| `Factory/Alias` | Typed creation through an alias. |
| `Factory/Offset` | Typed creation with a nonzero base offset. |
| `Factory/Virtual` | Typed creation through a virtual base; RTTI control. |
| `Factory/ErasedControl` | Existing untyped factory creation; this is not an old typed implementation. |
| `Names/Runtime` | One DSL element with an ID, long and short classes, and a duplicate class, using runtime name inputs. |
| `Names/Literal` | The same element using `UI::LiteralName` text and hashes. |
| `Tree/DSL` | C++ trees with names, classes, defaults, setters, and owned children. |
| `Tree/DSLLiteral` | Equivalent DSL trees with compile-time class metadata; generated IDs remain runtime strings. |
| `Tree/XMLControl` | Equivalent tree/value construction from a prepared XML string. |

Tree sizes are 8, 64, and 256 rows, with `1 + 3 * rows` controls per tree. The population
parameter adds 0 or 256 unrelated canonical registrations and attribute handlers. Setup,
input-string generation, verification, and registry cleanup are outside timed loops.
Creation and destruction are both timed. Controls have process-lifetime registrations;
population entries belong to a separate owner and are removed between cases.

There are 34 cases. For a literal-metadata comparison, use `Names/Runtime` versus
`Names/Literal` and `Tree/DSL` versus `Tree/DSLLiteral` from the same executable.
Run unchanged factory or XML cases in both arms as controls. This keeps binary
layout identical across arms. Verify text, hashes, and class order before timing;
`--verify` covers both authoring paths. Use longer measurement windows when the
platform CPU timer has coarse granularity.

## Compare revisions

Use an identical testbed in both arms and change only the production implementation being
evaluated. Record each source revision, dirty state, compiler, configuration, command,
and machine load beside the JSON. Google Benchmark records hardware/context, repetitions,
iteration counts, timing distributions, node throughput, and population counters.

For the conditional-cast baseline, keep this testbed and restore only the previous
`Create<T>` body in `ElementRegistration.h`, which always uses `dynamic_cast`. Build the
baseline and candidate in separate worktrees with the same Release settings.

Take the shared timing lock before measuring. Run the exact filtered case in fresh
processes: unregistering entries does not shrink hash-table capacity. First run A/A to
establish noise, then alternate A/B and B/A on a quiet machine. Use at least nine
repetitions and compare medians and spread; an effect inside the A/A spread is inconclusive.

```powershell
./UILayoutBenchmark.exe --benchmark_list_tests
./UILayoutBenchmark.exe --benchmark_filter='^Factory/Explicit/extra:0$' --benchmark_min_warmup_time=0.2 --benchmark_min_time=0.5s --benchmark_repetitions=9 --benchmark_out=C:/Temp/layout-baseline.json --benchmark_out_format=json
```

Repeat with the candidate binary and a different output filename. Google Benchmark's
comparison tools can consume these JSON files. Keep raw repetitions, not only aggregates.
For a cast change, virtual creation and untyped creation are controls; compare typed
explicit creation first, then inferred creation and DSL trees. Do not turn noisy timings
into CTest pass/fail thresholds or claim an editor frame-time gain from this testbed.
