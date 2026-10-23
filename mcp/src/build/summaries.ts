// The short records the build and test tools return in place of their
// output. Each holds what decides the next step (did it pass, which lines failed,
// where the full record is) and nothing an agent would only scroll past; the full
// build log and suite console go to a file whose path the record names.

/** Unique error lines a build summary lists; the count covers all of them. */
export const kMaxBuildErrorLines = 10;
/** Warning lines a build summary lists when the caller asks for them. */
export const kMaxBuildWarningLines = 20;
/** Failing tests, and skipped tests, a suite summary lists by name. */
export const kMaxListedTests = 20;
/** Characters kept of one error, failure or skip line. */
export const kMaxLineChars = 200;
/** Bytes of a registration's last output lines a summary of a run that is not a gtest suite keeps. */
export const kMaxOutputTailBytes = 1536;

// MSVC (error C2065:, error LNK2019:, error MSB3073:), clang and gcc (error:),
// and fatal errors. A file name containing "error" does not match: the word has
// to be followed by an optional code and a colon.
const kErrorLine = /\b(?:fatal\s+)?error\s*(?:[A-Z]{1,3}\d+)?\s*:/i;
const kWarningLine = /\bwarning\s*(?:[A-Z]{1,3}\d+)?\s*:/i;
// MSBuild repeats each diagnostic with the project it came from appended.
const kProjectSuffix = /\s*\[[^\]]*\.(?:vcxproj|csproj|proj)\]\s*$/i;

function clip(line: string): string {
  return line.length > kMaxLineChars ? `${line.slice(0, kMaxLineChars)}…` : line;
}

function uniqueMatches(lines: string[], pattern: RegExp): string[] {
  const seen = new Set<string>();
  for (const raw of lines) {
    if (!pattern.test(raw)) continue;
    seen.add(raw.replace(kProjectSuffix, "").trim());
  }
  return [...seen];
}

export interface BuildDiagnostics {
  errors: number;
  errorLines: string[];
  warnings: number;
  /** Present only when the caller asked for the warning lines. */
  warningLines?: string[];
}

/** Counts the unique error and warning lines of a build's output. */
export function summarizeBuildOutput(output: string, includeWarnings: boolean): BuildDiagnostics {
  const lines = output.split(/\r?\n/);
  const errors = uniqueMatches(lines, kErrorLine);
  const warnings = uniqueMatches(lines, kWarningLine).filter((line) => !kErrorLine.test(line));
  const result: BuildDiagnostics = {
    errors: errors.length,
    errorLines: errors.slice(0, kMaxBuildErrorLines).map(clip),
    warnings: warnings.length,
  };
  if (includeWarnings) result.warningLines = warnings.slice(0, kMaxBuildWarningLines).map(clip);
  return result;
}

export interface BuildRecord extends BuildDiagnostics {
  exitCode: number | null;
  timedOut: boolean;
  durationSec: number;
  targets: string[];
  preset: string;
  config: string;
  /** Executables and libraries written while the build ran, relative to the build directory. */
  binaries: string[];
  binaryCount: number;
  logPath: string;
}

/** The build summary as text: the exit, the counts, the first unique errors, the binaries, the log. */
export function formatBuildRecord(record: BuildRecord): string {
  const ok = record.exitCode === 0;
  const exit = record.timedOut ? "timed out" : `exit ${record.exitCode}`;
  const lines = [
    `BUILD ${ok ? "OK" : "FAILED"}: ${exit} in ${record.durationSec.toFixed(1)} s; targets ${record.targets.join(" ")}; ${record.preset} ${record.config}`,
    `errors ${record.errors}, warnings ${record.warnings}${record.warnings > 0 && !record.warningLines ? " (includeWarnings=true lists them)" : ""}`,
  ];
  if (record.errors > 0) {
    lines.push(`first ${record.errorLines.length} unique error line(s):`);
    for (const line of record.errorLines) lines.push(`  ${line}`);
  }
  if (record.warningLines?.length) {
    lines.push(`first ${record.warningLines.length} unique warning line(s):`);
    for (const line of record.warningLines) lines.push(`  ${line}`);
  }
  const more = record.binaryCount > record.binaries.length ? ` (+${record.binaryCount - record.binaries.length} more)` : "";
  lines.push(`binaries written: ${record.binaryCount}${record.binaries.length ? ` — ${record.binaries.join(", ")}${more}` : ""}`);
  lines.push(`log: ${record.logPath}`);
  return lines.join("\n");
}

const kEntities: Record<string, string> = { amp: "&", lt: "<", gt: ">", quot: "\"", apos: "'" };

function decodeXml(text: string): string {
  return text.replace(/&(#x[0-9a-f]+|#\d+|[a-z]+);/gi, (whole, body: string) => {
    if (body[0] === "#") {
      const code = body[1] === "x" || body[1] === "X" ? parseInt(body.slice(2), 16) : parseInt(body.slice(1), 10);
      return Number.isFinite(code) ? String.fromCodePoint(code) : whole;
    }
    return kEntities[body.toLowerCase()] ?? whole;
  });
}

function attribute(attributes: string, name: string): string | undefined {
  const match = attributes.match(new RegExp(`\\b${name}="([^"]*)"`));
  return match ? decodeXml(match[1]) : undefined;
}

// gtest writes a failure or skip message as "<file>:<line>" (or "unknown file" for
// an exception outside an assertion) on its own line and the text below it; the
// location alone does not say what went wrong, so the two are joined.
function firstMessageLine(message: string | undefined): string {
  const lines = (message ?? "").split(/\r?\n/).map((line) => line.trim()).filter((line) => line.length > 0);
  if (lines.length === 0) return "no message recorded";
  if (lines.length > 1 && /:\d+$|^unknown file$/.test(lines[0])) return clip(`${lines[0]}: ${lines[1]}`);
  return clip(lines[0]);
}

export interface SuiteRecord {
  suite: string;
  /** False for a registration that is not a gtest suite: it is judged by its exit code alone. */
  gtest: boolean;
  exitCode: number | null;
  timedOut: boolean;
  durationSec: number;
  /** Absent when the suite wrote no XML (it crashed before the record, or is not a gtest suite). */
  declared?: number;
  ran?: number;
  disabled?: number;
  failed?: number;
  skipped?: number;
  failures: string[];
  skips: string[];
  filter?: string;
  /** The last lines of the output of a run that is not a gtest suite, oldest first. */
  outputTail?: string[];
  /** Absent for a run that is not a gtest suite. */
  xmlPath?: string;
  logPath: string;
}

/**
 * Reads a gtest XML report: declared = ran + disabled, the failing tests with
 * their first message line, the skipped tests with their reason.
 */
export function summarizeGtestXml(xml: string): Pick<SuiteRecord, "declared" | "ran" | "disabled" | "failed" | "skipped" | "failures" | "skips"> {
  const root = xml.match(/<testsuites\b([^>]*)>/);
  const rootAttributes = root ? root[1] : "";
  const declared = Number(attribute(rootAttributes, "tests") ?? 0);
  const disabled = Number(attribute(rootAttributes, "disabled") ?? 0);
  const failures: string[] = [];
  const skips: string[] = [];
  let ran = 0;
  let failed = 0;
  const testcase = /<testcase\b([^>]*?)(?:\/>|>([\s\S]*?)<\/testcase>)/g;
  for (const match of xml.matchAll(testcase)) {
    const attributes = match[1];
    const body = match[2] ?? "";
    const status = attribute(attributes, "status");
    const result = attribute(attributes, "result");
    if (status === "notrun" && result !== "skipped") continue;
    ran++;
    const name = `${attribute(attributes, "classname") ?? "?"}.${attribute(attributes, "name") ?? "?"}`;
    const skip = body.match(/<skipped\b([^>]*?)\/?>/);
    if (skip || result === "skipped") {
      skips.push(`${name}: ${firstMessageLine(skip ? attribute(skip[1], "message") : undefined)}`);
      continue;
    }
    const failure = body.match(/<failure\b([^>]*?)\/?>/);
    if (failure) {
      failed++;
      failures.push(`${name}: ${firstMessageLine(attribute(failure[1], "message"))}`);
    }
  }
  return { declared, ran, disabled, failed, skipped: skips.length, failures, skips };
}

/**
 * True for a gtest run that exited 0, ran at least one test and failed none, or for
 * another registration that exited 0.
 */
export function isPassingSuite(record: SuiteRecord): boolean {
  if (!record.gtest) return record.exitCode === 0;
  return record.exitCode === 0 && (record.ran ?? 0) > 0 && (record.failed ?? 0) === 0;
}

/** The last lines of `output`, each clipped, that fit in kMaxOutputTailBytes; oldest first. */
export function outputTail(output: string): string[] {
  const lines = output.split(/\r?\n/).map((line) => line.trimEnd()).filter((line) => line.length > 0);
  const tail: string[] = [];
  let bytes = 0;
  for (let i = lines.length - 1; i >= 0; i--) {
    const line = clip(lines[i]);
    bytes += Buffer.byteLength(line) + 1;
    if (bytes > kMaxOutputTailBytes) break;
    tail.unshift(line);
  }
  return tail;
}

/** The suite summary as text: declared = ran + disabled, the failures, the skips, the paths. */
export function formatSuiteRecord(record: SuiteRecord): string {
  const exit = record.timedOut ? "timed out" : `exit ${record.exitCode}`;
  const lines: string[] = [];
  if (!record.gtest) {
    lines.push(`TESTS ${isPassingSuite(record) ? "PASSED" : "FAILED"} ${record.suite}: ${exit} in ${record.durationSec.toFixed(1)} s; not a gtest suite, so no counts${record.outputTail?.length ? "; the last lines of its output:" : ""}`);
    for (const line of record.outputTail ?? []) lines.push(`  ${line}`);
    lines.push(`log: ${record.logPath}`);
    return lines.join("\n");
  }
  if (record.ran === 0) {
    const reason = record.filter ? "the filter matched no test" : "the suite declared no test";
    lines.push(`TESTS VOID ${record.suite}: ${reason}; ${exit} in ${record.durationSec.toFixed(1)} s; a run with no test is not a pass`);
  } else if (record.declared === undefined) {
    lines.push(`TESTS FAILED ${record.suite}: ${exit} in ${record.durationSec.toFixed(1)} s; no XML was written (the suite crashed before its record); the log holds the console`);
  } else {
    const ok = isPassingSuite(record);
    lines.push(`TESTS ${ok ? "PASSED" : "FAILED"} ${record.suite}: ${exit} in ${record.durationSec.toFixed(1)} s; declared ${record.declared} = ran ${record.ran} + disabled ${record.disabled}; failed ${record.failed}; skipped ${record.skipped}`);
  }
  if (record.filter) lines.push(`filter ${record.filter}: a filtered run is a red-first check, not a gate`);
  for (const failure of record.failures.slice(0, kMaxListedTests)) lines.push(`  FAIL ${failure}`);
  if (record.failures.length > kMaxListedTests) lines.push(`  … ${record.failures.length - kMaxListedTests} more failing test(s) in the XML`);
  for (const skip of record.skips.slice(0, kMaxListedTests)) lines.push(`  skip ${skip}`);
  if (record.skips.length > kMaxListedTests) lines.push(`  … ${record.skips.length - kMaxListedTests} more skipped test(s) in the XML`);
  lines.push(`xml: ${record.xmlPath}`);
  lines.push(`log: ${record.logPath}`);
  return lines.join("\n");
}
