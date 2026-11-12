---
name: root-cause-debugging
description: Method for debugging to true root cause — reproduce first, verify the instrument before the reading, instrument before concluding, one variable at a time, and don't stop at the first real bug. Use for any defect hunt - crashes, hangs, wrong output, flaky tests, perf regressions.
---

# Root-cause debugging

1. **Reproduce before you theorize.** A bug you can't trigger is a rumor. Capture the failing state (logs, dumps, exact binaries, inputs) BEFORE restarting or rebuilding — the crime scene is evidence you cannot recreate. A crash you let vanish uncaptured is "noted only", not investigated.
2. **Verify the instrument before the reading.** Logs rotate and flood; counters include things you didn't expect (a "GPU time" that includes the vsync wait); test filters can match nothing and exit green; screenshots can be stale frames. Prove your measurement channel sees what you think it sees — inject a known event and watch it appear — before trusting any reading from it.
3. **Start with data, not logic.** Most wrong-output bugs are the system reading different data than you think (wrong file resolved, stale cache served, wrong buffer bound, dead config path) — not wrong math. Write a known value at the source and check whether the output changes before auditing the transform.
4. **Bisect by layer with known values.** For any pipeline (input → transforms → output), inject a known value at the midpoint rather than reading all the code end-to-end. Halve the suspect region each time.
5. **One variable at a time.** Simultaneous changes mask each other — especially when debugging conventions, signs, and orderings. Change one thing, test, revert if it didn't explain the symptom.
6. **Separate environment from code.** Before blaming the code, enumerate the environmental suspects: stale binaries (delete and rebuild — never trust timestamps), concurrent processes sharing resources, degraded external state (drivers, caches, daemons), configuration drift. An environmental cause pattern-matches a code bug perfectly until you list them explicitly.
7. **Attribute before you fix.** For hangs, stalls, and perf: build the attribution first — who is blocked, on what, since when (watchdogs, phase timers, stack captures, dumps). A fix chosen before attribution is a guess with a diff attached. One reported symptom can decompose into several independent mechanisms; attribution is what finds them all.
8. **Test the counter-hypothesis quantitatively.** State what your hypothesis predicts AND what the leading alternative predicts, then measure. A result that matches one and misses the other by a wide margin is proof; a measurement consistent with both is not evidence yet.
9. **A found bug is not the reported bug.** Fix what you find, but keep hunting until the reported symptom is explained by a mechanism you can demonstrate. Confirm you are looking at the object/case the reporter meant before diagnosing.
10. **Intermittent means under-instrumented.** Make it deterministic (injected timing hooks, stress amplification) or make it loud (a watchdog that captures state at trigger). "Couldn't reproduce" closes nothing — it queues instrumentation.
11. **Measure the absence.** After the fix, re-run the original repro and show the symptom gone under the same measurement — plus what did NOT change. A before/after table beats "seems fixed".

## This engine's instruments

Several editors are often alive: verify PID and path before attaching, and never kill an editor you did not launch.

- **Native crashes and hangs:** cdb from the Windows SDK Debugging Tools (usually `C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\`). Snapshot a running process without stopping it: `cdb -pv -p <pid> -c "~*k; qd"`. Reproduce under the debugger: `cdb -g -G <exe> <args>`, then `!analyze -v`, `~*k`, `.frame N` and `dv /t`. Post-mortem: `cdb -z <dump> -c "!analyze -v; ~*k; q"`. PDBs sit beside the staged binaries.
- **Memory bugs:** the `vs2026-x64-local-asan` preset (`macos-arm64-local-asan` on macOS), Debug configuration, ASan runtime DLLs staged automatically on Windows, `GE_ENABLE_ASAN=1` defined so perf-sensitive tests can relax thresholds. Data races: `macos-arm64-local-tsan` (ThreadSanitizer, Debug, tests on). The commands are in `BUILD.md` (Sanitizers).
- **GPU:** RenderDoc and the Vulkan instruments, in `.claude/rules/rendering.md` and the `renderdoc-gpu-debug` skill (a third-party skill installed per machine, not part of the repository).
- **Live editor:** the MCP tools (`mcp__gameengine__*`, setup in `docs/getting-started/mcp-setup.mdx`) and the CLI `node mcp/ge.mjs <tool>` (`--help` lists all; `--port` or `GE_EDITOR_DEBUG_PORT` targets a non-default editor; `ge raw <method> '{...}'` reaches methods with no tool). A `get_log` reply names the next call in a `next` line (on stderr in the CLI, so stdout holds only the answer): paste it for the next call. A backgrounded or occluded editor window may render black or throttled; bring it to the foreground before drawing visual conclusions.
