// How a test is registered with CTest: the program, its arguments, the working
// directory and the environment its registration gives it. run_tests runs a
// registration as CTest would. A gtest suite runs in one process; named by its
// executable, it runs whole, whatever way it registered (one test, per test case, or
// under aliases). Any other registration (dotnet test, a node or python script, a
// cmake script) runs its registered command unchanged.

import * as path from "path";

/** Close names a refusal lists for a suite that is not registered. */
export const kMaxCloseMatches = 8;

export interface SuiteRun {
  program: string;
  /** The registered arguments; for a gtest suite these are the arguments to the test executable. */
  args: string[];
  /** True when the program is a gtest executable, so the run writes a gtest XML and takes a filter. */
  gtest: boolean;
  cwd: string;
  /** Variables the registration sets, applied over the caller's environment. */
  env: Record<string, string>;
}

export type SuiteResolution =
  | { kind: "run"; run: SuiteRun }
  /** `target` is the build target that produces the suite, when the registry names it. */
  | { kind: "notBuilt"; target?: string }
  | { kind: "unknown"; closeMatches: string[] };

interface Registration {
  name: string;
  command?: string[];
  properties?: { name: string; value: unknown }[];
}

function property(test: Registration, name: string): unknown {
  return test.properties?.find((p) => p.name === name)?.value;
}

function stem(file: string): string {
  return path.basename(file).replace(/\.exe$/i, "");
}

// A registration that runs the suite through cmake/RunTestExecutable.cmake names the
// executable, the working directory and the arguments in -D arguments.
function scriptArgument(command: string[], name: string): string | undefined {
  const prefix = `-D${name}=`;
  return command.find((arg) => arg.startsWith(prefix))?.slice(prefix.length);
}

const kGtestFlag = /^--gtest_/;
const kGtestFilterFlag = /^--gtest_filter=/;

function runOf(test: Registration): SuiteRun {
  const command = test.command!;
  const wrapped = /^cmake(\.exe)?$/i.test(path.basename(command[0])) ? scriptArgument(command, "TEST_EXE") : undefined;
  const scriptArgs = wrapped ? scriptArgument(command, "TEST_ARGS") : undefined;
  const args = wrapped ? (scriptArgs ? scriptArgs.split(";").filter((arg) => arg.length > 0) : []) : command.slice(1);
  // A program run directly is a gtest suite when it registers no argument but gtest
  // flags; dotnet, node, python and cmake registrations all name a project or a script.
  const gtest = wrapped !== undefined || args.every((arg) => kGtestFlag.test(arg));
  const program = wrapped ?? command[0];
  const registeredCwd = property(test, "WORKING_DIRECTORY");
  const cwd = (wrapped ? scriptArgument(command, "TEST_WORKING_DIR") : undefined)
    ?? (typeof registeredCwd === "string" ? registeredCwd : path.dirname(program));
  const env: Record<string, string> = {};
  const variables = property(test, "ENVIRONMENT");
  for (const entry of Array.isArray(variables) ? variables : []) {
    const text = String(entry);
    const equals = text.indexOf("=");
    if (equals > 0) env[text.slice(0, equals)] = text.slice(equals + 1);
  }
  return { program, args, gtest, cwd, env };
}

function gtestExecutableStem(test: Registration): string | undefined {
  if (!test.command?.length) return undefined;
  const run = runOf(test);
  return run.gtest ? stem(run.program) : undefined;
}

// A registered variant of a suite (`<Suite>_<Variant>`) is built by the suite's own
// target; a `<Target>_NOT_BUILT` placeholder names its target.
function buildTargetOf(tests: Registration[], name: string): string | undefined {
  if (name.endsWith("_NOT_BUILT")) return name.slice(0, -"_NOT_BUILT".length);
  const parts = name.split("_");
  for (let count = parts.length - 1; count > 0; count--) {
    const prefix = parts.slice(0, count).join("_");
    if (tests.some((t) => t.name === prefix)) return prefix;
  }
  return undefined;
}

/**
 * Finds `suite` in the output of `ctest --show-only=json-v1`: a registration of that
 * name, run with its registered arguments, or else the first registration whose gtest
 * executable is named `suite` (a suite registered per test case or under aliases),
 * run whole without its per-case filter.
 */
export function resolveSuiteRun(showOnly: { tests?: Registration[] }, suite: string): SuiteResolution {
  const tests = showOnly.tests ?? [];
  const named = tests.find((t) => t.name === suite);
  if (named?.command?.length) return { kind: "run", run: runOf(named) };
  const byExecutable = tests.find((t) => gtestExecutableStem(t) === suite);
  if (byExecutable) {
    const run = runOf(byExecutable);
    return { kind: "run", run: { ...run, args: run.args.filter((arg) => !kGtestFilterFlag.test(arg)) } };
  }
  if (named) {
    const target = buildTargetOf(tests, suite);
    return target ? { kind: "notBuilt", target } : { kind: "notBuilt" };
  }
  if (tests.some((t) => t.name === `${suite}_NOT_BUILT`)) return { kind: "notBuilt", target: suite };

  const wanted = suite.toLowerCase();
  const close = new Set<string>();
  for (const test of tests) {
    for (const candidate of [gtestExecutableStem(test), test.name.replace(/_NOT_BUILT$/, "")]) {
      if (!candidate) continue;
      const lower = candidate.toLowerCase();
      if (lower.includes(wanted) || wanted.includes(lower)) close.add(candidate);
    }
  }
  return { kind: "unknown", closeMatches: [...close].sort().slice(0, kMaxCloseMatches) };
}

/**
 * The arguments `run_tests` passes for `run`: a gtest suite's registered arguments
 * with the XML report and the caller's filter after them; another registration's
 * arguments unchanged. A filter on a registration that is not a gtest suite is refused.
 */
export function suiteArguments(run: SuiteRun, suite: string, xmlPath: string, filter?: string): { args: string[] } | { refusal: string } {
  if (!run.gtest) {
    if (filter) {
      return { refusal: `${suite} is not a gtest suite (it runs ${path.basename(run.program)} ${run.args.join(" ")}), so it takes no filter. Run it without filter.` };
    }
    return { args: run.args };
  }
  return { args: [...run.args, `--gtest_output=xml:${xmlPath}`, ...(filter ? [`--gtest_filter=${filter}`] : [])] };
}
