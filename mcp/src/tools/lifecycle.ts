import { execFile } from "child_process";
import * as fs from "fs";
import * as path from "path";
import { promisify } from "util";
import { z } from "zod";
import { freeDebugPort } from "../build/debug-port.js";
import { defaultCaptureDir, launchEditorProcess, type RenderDocLaunch, resolveRenderdoccmd, waitForEditorReady } from "../build/editor-process.js";
import { resolveSuiteRun, suiteArguments, type SuiteRun } from "../build/ctest-registry.js";
import { binaryDirectory, buildArgs, buildDirectory, detectPreset, editorExePath, normalizePreset } from "../build/preset.js";
import { runStamp, runToLog } from "../build/process.js";
import { type BuildRecord, formatBuildRecord, formatSuiteRecord, isPassingSuite, outputTail, type SuiteRecord, summarizeBuildOutput, summarizeGtestXml } from "../build/summaries.js";
import { repoRoot } from "../paths.js";
import { defineTool, type ToolDef, ToolError } from "../registry.js";

const execFileAsync = promisify(execFile);

const kDefaultBuildJobs = 12;
const kBuildTimeoutMs = 300_000;
const kSuiteTimeoutMs = 1_800_000;
const kCtestListTimeoutMs = 60_000;
// Bytes read from the end of a suite log for the last lines of a run that is not a gtest suite.
const kLogTailReadBytes = 16_384;
// The ctest listing of every registration runs to a few megabytes on a full tree.
const kCtestListMaxBytes = 64 * 1024 * 1024;
// Under the build directory: the full build logs and suite records the summaries point at.
const kToolLogDirectory = "ToolLogs";
// Runs whose log (and suite XML) each tool keeps in kToolLogDirectory; older runs are deleted.
const kToolLogRunsKept = 20;
// How deep under bin/<config> a built executable or library sits (Apps/Editor, Tests).
const kBinaryScanDepth = 3;
// Binaries a build summary names; the count covers all of them.
const kMaxListedBinaries = 10;
const kBinaryExtensions = new Set([".exe", ".dll", ".dylib", ".so"]);

/** Executables and libraries under `dir` modified at or after `sinceMs`, relative to `root`. */
function binariesWrittenSince(dir: string, root: string, sinceMs: number, depth = 0): string[] {
  let entries: fs.Dirent[];
  try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch { return []; }
  const found: string[] = [];
  for (const entry of entries) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      if (depth < kBinaryScanDepth) found.push(...binariesWrittenSince(full, root, sinceMs, depth + 1));
    } else if (kBinaryExtensions.has(path.extname(entry.name).toLowerCase())) {
      try { if (fs.statSync(full).mtimeMs >= sinceMs) found.push(path.relative(root, full)); } catch { /* removed mid-scan */ }
    }
  }
  return found;
}

/** Deletes all but the newest kToolLogRunsKept runs whose files in `dir` start with `prefix`. */
function pruneToolLogs(dir: string, prefix: string): void {
  let names: string[];
  try { names = fs.readdirSync(dir).filter((name) => name.startsWith(prefix)); } catch { return; }
  const byRun = new Map<string, string[]>();
  for (const name of names) {
    const run = path.parse(name).name;
    byRun.set(run, [...(byRun.get(run) ?? []), name]);
  }
  const modified = (run: string) => fs.statSync(path.join(dir, byRun.get(run)![0])).mtimeMs;
  const newestFirst = [...byRun.keys()].sort((a, b) => modified(b) - modified(a));
  for (const run of newestFirst.slice(kToolLogRunsKept)) {
    for (const name of byRun.get(run)!) fs.rmSync(path.join(dir, name), { force: true });
  }
}

/** The program, arguments, working directory and environment CTest registered for `suite`. */
async function registeredSuiteRun(buildDir: string, config: string, suite: string): Promise<SuiteRun> {
  let listing: string;
  try {
    ({ stdout: listing } = await execFileAsync("ctest", ["--test-dir", buildDir, "-C", config, "--show-only=json-v1"],
      { cwd: repoRoot(), timeout: kCtestListTimeoutMs, maxBuffer: kCtestListMaxBytes }));
  } catch (err: any) {
    const reason = String(err?.message ?? err).split(/\r?\n/)[0];
    throw new ToolError(`ctest could not list the tests in ${buildDir} (${reason}). Configure the build with BUILD_TESTING=ON.`);
  }
  const resolution = resolveSuiteRun(JSON.parse(listing), suite);
  if (resolution.kind === "notBuilt") {
    const build = resolution.target ? `build_engine target=${resolution.target}` : "build_engine and the test target that produces it";
    throw new ToolError(`${suite} is registered but not built for ${config}. Build it first with ${build}.`);
  }
  if (resolution.kind === "unknown") {
    const close = resolution.closeMatches.length ? ` Close names: ${resolution.closeMatches.join(", ")}.` : "";
    throw new ToolError(`No test suite named ${suite} is registered in ${buildDir}.${close}`);
  }
  const { program, gtest } = resolution.run;
  if (!fs.existsSync(program)) {
    throw new ToolError(gtest
      ? `No test executable at ${program}. Build it first with build_engine target=${path.parse(program).name}.`
      : `${suite} runs ${program}, which does not exist. Reconfigure the build so its registration names a program on this machine.`);
  }
  return resolution.run;
}

/** The last lines of the log at `logPath`, read from its last kLogTailReadBytes. */
function logTail(logPath: string): string[] {
  const fd = fs.openSync(logPath, "r");
  try {
    const size = fs.fstatSync(fd).size;
    const length = Math.min(size, kLogTailReadBytes);
    const buffer = Buffer.alloc(length);
    fs.readSync(fd, buffer, 0, length, size - length);
    return outputTail(buffer.toString("utf8"));
  } finally {
    fs.closeSync(fd);
  }
}

/** Builds one target with the output in a log file, and reads the log into a build record. */
async function buildTarget(preset: string, config: string, target: string, jobs: string, includeWarnings: boolean): Promise<BuildRecord> {
  const root = repoRoot();
  const logPath = path.join(root, buildDirectory(preset), kToolLogDirectory, `build-${target}-${runStamp()}.log`);
  // Whole seconds: some file systems store modification times at that resolution.
  const startMs = Math.floor(Date.now() / 1000) * 1000;
  const run = await runToLog("cmake", buildArgs(preset, config, target, jobs), { cwd: root, logPath, timeoutMs: kBuildTimeoutMs });
  pruneToolLogs(path.dirname(logPath), `build-${target}-`);
  const binDir = binaryDirectory(preset, config);
  const binaries = binariesWrittenSince(binDir, binDir, startMs);
  return {
    exitCode: run.exitCode,
    timedOut: run.timedOut,
    durationSec: run.durationMs / 1000,
    targets: [target],
    preset,
    config,
    ...summarizeBuildOutput(fs.readFileSync(logPath, "utf8"), includeWarnings),
    binaries: binaries.slice(0, kMaxListedBinaries),
    binaryCount: binaries.length,
    logPath,
  };
}

// Building and launching happen on this host, not in the editor — these are the
// only tools in the registry with no debug-server method behind them.

export const tools: ToolDef[] = [
  defineTool({
    name: "build_engine",
    category: "lifecycle",
    actsOnHost: true,
    description: "Build one CMake target and return the build summary: the exit code, the duration, errors N with the first ten unique error lines, warnings N (the lines only with includeWarnings=true), the binaries the build wrote, and the path of the full log. The summary is the record of the build: read it instead of the log, and do not call get_log after a build, because the errors are already here. To look for one line in the full log, search the log file. Auto-detects platform (Windows/macOS/Linux), architecture (x64/ARM64), and Visual Studio version. IMPORTANT: Shut down the editor (shutdown_editor) before building to avoid PDB lock errors.",
    schema: {
      target: z.string().optional().describe("CMake target (default: Editor)"),
      config: z.string().optional().describe("Build config: DebugFast, Debug or Release (default: DebugFast)"),
      jobs: z.coerce.number().optional().describe("Parallel jobs (default: 12)"),
      preset: z.string().optional().describe("CMake preset (auto-detected from platform/arch/VS version if omitted)"),
      includeWarnings: z.boolean().optional().describe("List the first unique warning lines as well as their count (default false: the count only)"),
    },
    run: async ({ target, config, jobs, preset, includeWarnings }) => {
      const t = target ?? "Editor";
      const c = config ?? "DebugFast";
      const p = normalizePreset(preset ?? detectPreset(c));
      const record = await buildTarget(p, c, t, String(jobs ?? kDefaultBuildJobs), includeWarnings ?? false);
      const summary = formatBuildRecord(record);
      if (record.exitCode !== 0) throw new ToolError(summary);
      return { data: record, summary };
    },
  }),

  defineTool({
    name: "run_tests",
    category: "lifecycle",
    actsOnHost: true,
    description: "Run a registered test (a gtest suite gets the test summary; another registration its exit code) with the arguments, working directory and environment its CTest registration gives it, and return its summary. A gtest suite runs in one process: declared = ran + disabled, failed N with each failing test and its first message line, skipped N with each skip and its reason, the path of the gtest XML and the path of the full console log. A run in which no test ran (a filter that matches nothing) is void and fails. Any other registration (dotnet test, a node, python or cmake script) runs its registered command and reports its exit code, the last lines of its output and the log path; it takes no filter. The summary and the XML are the record of the run: never read the console log to find a result, and do not call get_log after a suite. Build the suite first with build_engine target=<suite>.",
    schema: {
      suite: z.string().describe("A registered test suite such as EditorTests or ManagedCoreBridgeTests"),
      filter: z.string().optional().describe("gtest filter (--gtest_filter), gtest suites only, for a red-first run of one test or a repeat of one failing test only; a gate runs the suite whole"),
      config: z.string().optional().describe("Build config (default: DebugFast)"),
      preset: z.string().optional().describe("CMake preset (auto-detected if omitted)"),
    },
    run: async ({ suite, filter, config, preset }) => {
      const c = config ?? "DebugFast";
      const p = normalizePreset(preset ?? detectPreset(c));
      const buildDir = path.join(repoRoot(), buildDirectory(p));
      const run = await registeredSuiteRun(buildDir, c, suite);
      const logDir = path.join(buildDir, kToolLogDirectory);
      const stem = path.join(logDir, `${suite}-${runStamp()}`);
      const xmlPath = `${stem}.xml`;
      const logPath = `${stem}.log`;
      const invocation = suiteArguments(run, suite, xmlPath, filter);
      if ("refusal" in invocation) throw new ToolError(invocation.refusal);
      const result = await runToLog(run.program, invocation.args, { cwd: run.cwd, env: run.env, logPath, timeoutMs: kSuiteTimeoutMs });
      pruneToolLogs(logDir, `${suite}-`);
      const record: SuiteRecord = {
        suite,
        gtest: run.gtest,
        exitCode: result.exitCode,
        timedOut: result.timedOut,
        durationSec: result.durationMs / 1000,
        failures: [],
        skips: [],
        ...(run.gtest
          ? { ...(fs.existsSync(xmlPath) ? summarizeGtestXml(fs.readFileSync(xmlPath, "utf8")) : {}), xmlPath }
          : { outputTail: logTail(logPath) }),
        ...(filter ? { filter } : {}),
        logPath,
      };
      const summary = formatSuiteRecord(record);
      if (!isPassingSuite(record)) throw new ToolError(summary);
      return { data: record, summary };
    },
  }),

  defineTool({
    name: "launch_editor",
    category: "lifecycle",
    actsOnHost: true,
    description: "Launch the Editor process in the background. Frees the debug port first (graceful IPC shutdown of the editor holding the configured debug port — GE_EDITOR_DEBUG_PORT, default 9999 — force-kill only if it hangs); editors on other ports are left running. Returns immediately — use wait_for_editor to confirm it's ready. Set renderdoc=true to launch under RenderDoc so trigger_capture works; captures then land at <captureDir>/editor_frameN.rdc.",
    schema: {
      config: z.string().optional().describe("Build config: DebugFast, Debug or Release (default: DebugFast)"),
      preset: z.string().optional().describe("CMake preset (auto-detected if omitted)"),
      extraArgs: z.array(z.string()).optional().describe("Extra command-line args passed to Editor.exe"),
      renderdoc: z.boolean().optional().describe("Launch under 'renderdoccmd capture' so trigger_capture can capture frames. Defaults captureCompat=true, which drops VK_EXT_descriptor_buffer so the .rdc replays — but that also makes HZB occlusion culling report zero culled draws, so pass captureCompat=false for occlusion or perf captures."),
      captureCompat: z.boolean().optional().describe("renderdoc=true: set GE_VK_CAPTURE_COMPAT=1 (default true). Set false to keep VK_EXT_descriptor_buffer — occlusion/perf numbers stay trustworthy, but the capture may fail to replay."),
      captureDir: z.string().optional().describe("renderdoc=true: directory for .rdc files (default: <editor exe dir>/Captures). The editor is pointed at the same directory via GE_RENDERDOC_CAPTURE_DIR."),
    },
    run: async ({ config, preset, extraArgs, renderdoc, captureCompat, captureDir }) => {
      const c = config ?? "DebugFast";
      const p = normalizePreset(preset ?? detectPreset(c));
      const exe = editorExePath(p, c);

      if (!fs.existsSync(exe)) {
        throw new ToolError(`Editor not found at ${exe}. Build it first with build_engine.`);
      }

      let renderdocLaunch: RenderDocLaunch | undefined;
      if (renderdoc) {
        const renderdoccmd = resolveRenderdoccmd();
        if (!renderdoccmd) {
          throw new ToolError(process.env.RENDERDOC_PATH
            ? `RENDERDOC_PATH is '${process.env.RENDERDOC_PATH}' but no renderdoccmd executable is there. `
              + `Point it at the RenderDoc install directory, or at renderdoccmd itself.`
            : `renderdoccmd not found on PATH or at the default install. Install RenderDoc `
              + `(C:\\Program Files\\RenderDoc\\ on Windows), or set RENDERDOC_PATH to its install directory.`);
        }
        renderdocLaunch = {
          captureDir: captureDir ?? defaultCaptureDir(exe),
          renderdoccmd,
          captureCompat: captureCompat ?? true,
        };
      }

      // Free the debug port if a previous editor holds it. Editors on other
      // ports (e.g. the developer's own session) are not touched.
      const portResult = await freeDebugPort();
      if (portResult.blocked) throw new ToolError(portResult.blocked);

      const { pid, logFile, renderdoccmdLog } = launchEditorProcess(exe, extraArgs ?? [], renderdocLaunch);

      return { data: {
        launched: true,
        pid,
        exe,
        logFile,
        portSteps: portResult.steps,
        ...(renderdocLaunch
          ? {
              renderdoc: renderdocLaunch.renderdoccmd,
              captureDir: renderdocLaunch.captureDir,
              captureCompat: renderdocLaunch.captureCompat
                ? "GE_VK_CAPTURE_COMPAT=1 — descriptor buffer dropped so captures replay; HZB occlusion reports 0 culled, so do not use this session for occlusion or perf analysis"
                : "GE_VK_CAPTURE_COMPAT unset — occlusion/perf numbers are trustworthy, but the capture may fail to replay",
              // renderdoccmd launches and injects into the Editor, so the pid
              // above is the launcher's, not the Editor's.
              pidIsRenderdoccmd: true,
              // A failed launch/inject is reported only here.
              renderdoccmdLog,
            }
          : {}),
        note: "Editor is starting. Use wait_for_editor to confirm it is ready and the debug server is accepting connections.",
      } };
    },
  }),

  defineTool({
    name: "wait_for_editor",
    category: "lifecycle",
    description: "Wait until the editor's debug server is accepting connections on the configured debug port (GE_EDITOR_DEBUG_PORT, default 9999). Use after launch_editor or after manually starting the editor.",
    schema: {
      timeoutSec: z.coerce.number().optional().describe("Max seconds to wait (default: 30)"),
    },
    run: async ({ timeoutSec }) => {
      const timeout = (timeoutSec ?? 30) * 1000;
      const ready = await waitForEditorReady(timeout);
      if (!ready) throw new ToolError(`Timed out after ${timeout / 1000}s waiting for the editor. Is it running?`);
      return { data: { connected: true, waitedMs: ready.waitedMs, editorState: ready.state } };
    },
  }),

  defineTool({
    name: "editor_full_cycle",
    category: "lifecycle",
    actsOnHost: true,
    description: "Full build-launch-verify cycle: frees the debug port (graceful shutdown of the editor on the configured debug port only), builds the Editor target, launches it, and waits for it to be ready. Returns editor state when ready. If an editor on another port holds this build's PDB, the build step reports the lock — stop that editor yourself (shutdown_editor with --port set to its debug port).",
    schema: {
      config: z.string().optional().describe("Build config: DebugFast, Debug or Release (default: DebugFast)"),
      target: z.string().optional().describe("CMake target (default: Editor)"),
      jobs: z.coerce.number().optional().describe("Parallel jobs (default: 12)"),
    },
    run: async ({ config, target, jobs }) => {
      const c = config ?? "DebugFast";
      const t = target ?? "Editor";
      const j = String(jobs ?? kDefaultBuildJobs);
      const p = detectPreset(c);
      const steps: string[] = [];

      // Step 1: Free the debug port (port-scoped, like launch_editor — never
      // sweeps editors on other ports). If a different editor still holds this
      // build's PDB, the build error below reports it rather than us killing
      // a session we don't own.
      const portResult = await freeDebugPort();
      steps.push(...portResult.steps);
      if (portResult.blocked) throw new ToolError(portResult.blocked);

      // Step 2: Build.
      steps.push(`Building ${t} (${c})...`);
      const build = await buildTarget(p, c, t, j, false);
      if (build.exitCode !== 0) throw new ToolError(formatBuildRecord(build));
      steps.push(`Build succeeded in ${build.durationSec.toFixed(1)} s.`);

      // Step 3: Launch editor.
      const exe = editorExePath(p, c);
      if (!fs.existsSync(exe)) throw new ToolError(`Editor exe not found at ${exe} after build.`);
      const { pid } = launchEditorProcess(exe);
      steps.push(`Launched editor (PID ${pid}).`);

      // Step 4: Wait for connection.
      const ready = await waitForEditorReady(45_000);
      if (!ready) {
        steps.push("Timed out waiting for editor connection.");
        throw new ToolError(JSON.stringify({ steps }, null, 2));
      }
      steps.push(`Editor ready after ${Math.round(ready.waitedMs / 1000)}s.`);
      return { data: { steps, editorState: ready.state } };
    },
  }),

  defineTool({
    name: "shutdown_editor",
    category: "lifecycle",
    actsOnHost: true,
    // Drives `shutdown` over IPC before force-killing, so the drift check counts
    // that method as covered even though there is no plain proxy for it.
    coversIpcMethods: ["shutdown"],
    description: "Shut down the editor holding the configured debug port (GE_EDITOR_DEBUG_PORT / --port, default 9999) — graceful IPC shutdown first, then force-kill of that port's holder only if it hangs. Editors on other ports (other worktrees, other sessions) are never touched, and a non-Editor port holder is never killed. Use it to release PDB locks before rebuilding; to stop a different editor, target its port with --port.",
    schema: {},
    run: async () => {
      const result = await freeDebugPort();
      if (result.blocked) throw new ToolError(result.blocked);
      return { data: { steps: result.steps } };
    },
  }),
];
