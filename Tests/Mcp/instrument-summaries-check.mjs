// The agent-facing instruments answer with summaries, not output. An agent that
// drives the engine through the MCP server or the ge CLI sees the tool descriptions
// and these summaries, not the engine's source, so both are the contract.
//
// Arms:
//   * take_screenshot answers with the record line (path, size, SHA-256, pose,
//     validation counts) and sends the image inline only with includeImage;
//   * get_log forwards since and pattern and leaves the defaults to the editor;
//   * every get_log reply ends with the next call: since=<nextSince> with the same level
//     and pattern, the narrower call when the cap left lines out, the same line after
//     'unchanged'; the MCP server's reply carries it as `next`, the CLI prints it as its
//     last line, and a value holding a single quote is double-quoted with bash escapes;
//   * a gtest XML reads as declared = ran + disabled, the failures with their first
//     message line and the skips with their reason;
//   * a run in which no test ran (a filter that matches nothing) reads as void, never
//     as passed;
//   * run_tests takes a suite's executable, working directory and environment from
//     its CTest registration, whether it registered once, per test case or through
//     cmake/RunTestExecutable.cmake, and names close matches for an unknown suite;
//   * a registration that is not a gtest suite (dotnet test, a node script) runs its
//     registered command with no gtest flag, refuses a filter, and reads as its exit
//     code and its last output lines;
//   * a registered variant that is not built names its suite's build target;
//   * the registered environment reaches the process run_tests starts;
//   * a build log reads as the error and warning counts with the first ten unique
//     error lines, and the summary of a log with many errors stays under 2 KB;
//   * the descriptions of get_log, build_engine, run_tests and the capture tools, and
//     the CLI overview, carry the guidance: summaries are the record, get_log takes a
//     level and a pattern and since, and is not polled.
//
// The editor is a fake TCP server speaking the debug protocol; no editor binary.
// Runs the built server modules (mcp/dist), which ge.mjs brings up to date first.
//
// Exit 0 = every arm held; 1 = a failure; 2 = the harness could not run.
//
//   node Tests/Mcp/instrument-summaries-check.mjs

import { spawn, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const dist = path.join(repoRoot, 'mcp', 'dist');

const prebuild = spawnSync(process.execPath, [path.join(repoRoot, 'mcp', 'ge.mjs'), '--help'],
                           { cwd: repoRoot, stdio: ['ignore', 'ignore', 'inherit'] });
if (prebuild.status !== 0 || !fs.existsSync(path.join(dist, 'tools', 'index.js'))) {
    console.error(`could not build ${dist} (ge.mjs exit ${prebuild.status})`);
    process.exit(2);
}

const failures = [];
function check(label, condition, detail) {
    if (!condition) failures.push(`${label}\n    ${detail}`);
}

// A tiny PNG stand-in: the record hashes the bytes the editor sent, whatever they are.
const pngBytes = Buffer.from('not really a png, but bytes to hash');
const received = [];
let logReply = { lines: [], nextSince: 0 };
const fake = net.createServer((socket) => {
    let buffer = '';
    socket.on('data', (chunk) => {
        buffer += chunk.toString();
        const lines = buffer.split('\n');
        buffer = lines.pop() ?? '';
        for (const line of lines) {
            if (!line.trim()) continue;
            const request = JSON.parse(line);
            received.push(request);
            const result = request.method === 'take_screenshot'
                ? { filePath: 'C:/captures/screenshot_window_1.png', pngBase64: pngBytes.toString('base64'),
                    width: 64, height: 32, target: 'window', method: 'rendergraph', pendingMaterialTextureBinds: 0,
                    pose: { position: [1, 2, 3], yawDeg: 45, pitchDeg: -10 }, validation: { errors: 0, warnings: 2 } }
                : logReply;
            socket.write(JSON.stringify({ id: request.id, ok: true, result }) + '\n');
        }
    });
    socket.on('error', () => { /* the client hangs up at the end */ });
});
await new Promise((resolve) => fake.listen(0, '127.0.0.1', resolve));
process.env.GE_EDITOR_DEBUG_PORT = String(fake.address().port);

try {
    const { allTools } = await import(pathToFileURL(path.join(dist, 'tools', 'index.js')).href);
    const tool = (name) => allTools.find((t) => t.name === name);

    // take_screenshot: the record line, no inline image unless asked.
    const shot = await tool('take_screenshot').run({});
    const sha = createHash('sha256').update(pngBytes).digest('hex');
    check('take_screenshot: no inline image by default', shot.image?.inline === false,
          `image.inline = ${shot.image?.inline}`);
    check('take_screenshot: the record line names size, hash, pose and validation',
          shot.summary.includes(`${pngBytes.length} bytes`) && shot.summary.includes(`sha256 ${sha}`)
              && shot.summary.includes('scene view pose (1.00, 2.00, 3.00) yaw 45.0 pitch -10.0')
              && shot.summary.includes('validation errors 0, warnings 2'),
          `summary: ${shot.summary}`);
    const inline = await tool('take_screenshot').run({ includeImage: true });
    check('take_screenshot: includeImage sends the image inline', inline.image?.inline === true,
          `image.inline = ${inline.image?.inline}`);
    check('take_screenshot: includeImage stays on this side',
          !('includeImage' in (received.at(-1)?.params ?? {})), `params: ${JSON.stringify(received.at(-1)?.params)}`);

    // get_log: the cursor and the pattern reach the editor; the defaults are the editor's.
    await tool('get_log').run({ since: 41, pattern: 'shader|pipeline' });
    check('get_log: since and pattern are forwarded, nothing else',
          JSON.stringify(received.at(-1)?.params) === JSON.stringify({ since: 41, pattern: 'shader|pipeline' }),
          `params: ${JSON.stringify(received.at(-1)?.params)}`);

    // get_log: the next call, ready to paste.
    const { mcpNextCall, cliNextCall } = await import(pathToFileURL(path.join(dist, 'next-call.js')).href);
    logReply = { lines: ['[12:00:01] [warning] shader x'], nextSince: 57 };
    const delta = await tool('get_log').run({ since: 41, minLevel: 'warning', pattern: 'shader|pipeline' });
    check('get_log: next is since=<nextSince> with the same level and pattern',
          JSON.stringify(delta.next) === JSON.stringify({ since: 57, minLevel: 'warning', pattern: 'shader|pipeline' }),
          `next: ${JSON.stringify(delta.next)}`);
    const mcpNext = delta.next ? mcpNextCall('get_log', delta.next) : 'no next';
    check('get_log: the MCP form of next',
          mcpNext === 'get_log since=57 minLevel="warning" pattern="shader|pipeline"', mcpNext);
    logReply = { unchanged: 'unchanged since 12:00:01Z; pass since=57', nextSince: 57 };
    const unchanged = await tool('get_log').run({ pattern: 'shader' });
    check('get_log: an unchanged reply still names the next call',
          JSON.stringify(unchanged.next) === JSON.stringify({ since: 57, pattern: 'shader' }),
          `next: ${JSON.stringify(unchanged.next)}`);
    logReply = { lines: ['a', 'b'], nextSince: 90, omitted: { overCap: 12, narrower: '12 matching line(s) did not fit' } };
    const capped = await tool('get_log').run({ since: 41 });
    check('get_log: lines left out by the cap make next the narrower call (a higher level, the same window)',
          JSON.stringify(capped.next) === JSON.stringify({ since: 41, minLevel: 'error' }),
          `next: ${JSON.stringify(capped.next)}`);
    const cappedAtError = await tool('get_log').run({ minLevel: 'error', offset: 4 });
    check('get_log: at error level the narrower call pages back past the lines returned',
          JSON.stringify(cappedAtError.next) === JSON.stringify({ minLevel: 'error', offset: 6 }),
          `next: ${JSON.stringify(cappedAtError.next)}`);

    // A value that holds a single quote is double-quoted with bash escapes.
    const quoted = cliNextCall('get_log', { pattern: "can't $x" });
    check('CLI next: a value holding a single quote is double-quoted with bash escapes',
          quoted.startsWith(`node mcp/ge.mjs get_log --pattern "can't \\$x"`), quoted);

    // The MCP server's text block holds next beside nextSince.
    logReply = { lines: ['[12:00:01] [warning] shader x'], nextSince: 57 };
    const mcpText = await new Promise((resolve) => {
        const server = spawn(process.execPath, [path.join(dist, 'index.js')], { cwd: repoRoot, env: process.env });
        let stdout = '';
        const timer = setTimeout(() => { server.kill(); resolve(`timed out: ${stdout.slice(0, 200)}`); }, 20000);
        server.stdout.on('data', (chunk) => {
            stdout += chunk;
            const answer = stdout.split('\n').find((line) => line.includes('"id":2'));
            if (!answer) return;
            clearTimeout(timer);
            server.kill();
            resolve(JSON.parse(answer).result?.content?.[0]?.text ?? answer);
        });
        const send = (message) => server.stdin.write(JSON.stringify({ jsonrpc: '2.0', ...message }) + '\n');
        send({ id: 1, method: 'initialize', params: { protocolVersion: '2024-11-05', capabilities: {},
                                                      clientInfo: { name: 'instrument-summaries-check', version: '1' } } });
        send({ method: 'notifications/initialized' });
        send({ id: 2, method: 'tools/call', params: { name: 'get_log', arguments: { pattern: 'shader' } } });
    });
    let mcpReply = null;
    try { mcpReply = JSON.parse(mcpText.split('\n\n[editor ')[0]); } catch { /* reported below */ }
    check('MCP get_log: the reply holds next beside nextSince',
          mcpReply?.nextSince === 57 && mcpReply?.next === 'get_log since=57 pattern="shader"', mcpText);

    // The CLI prints the next call as its last line, with the flags the call used.
    logReply = { lines: ['[12:00:01] [warning] shader x'], nextSince: 57 };
    const cli = await new Promise((resolve) => {
        const child = spawn(process.execPath, [path.join(repoRoot, 'mcp', 'ge.mjs'), 'get_log', '--since', '41',
                                               '--min-level', 'warning', '--pattern', 'shader|pipeline'],
                            { cwd: repoRoot, env: { ...process.env, GE_CLI_NO_BUILD: '1' } });
        let stdout = '';
        child.stdout.on('data', (chunk) => { stdout += chunk; });
        child.on('close', (code) => resolve({ code, stdout }));
    });
    const lastLine = cli.stdout.trimEnd().split(/\r?\n/).at(-1);
    check('CLI get_log: the last line is the next call to paste',
          cli.code === 0 && lastLine === `next: node mcp/ge.mjs get_log --since 57 --min-level warning --pattern 'shader|pipeline' --port ${fake.address().port}`,
          `exit ${cli.code}, last line: ${lastLine}`);

    const summaries = await import(pathToFileURL(path.join(dist, 'build', 'summaries.js')).href);

    // A gtest report: one pass, one failure, one skip, one disabled test.
    const xml = `<?xml version="1.0" encoding="UTF-8"?>
<testsuites tests="4" failures="1" disabled="1" errors="0" name="AllTests">
  <testsuite name="Log" tests="4" failures="1" disabled="1">
    <testcase name="Passes" status="run" result="completed" classname="Log" />
    <testcase name="Fails" status="run" result="completed" classname="Log">
      <failure message="C:\\src\\LogTests.cpp:12&#x0A;Expected equality of these values:&#x0A;  a" type=""><![CDATA[details]]></failure>
    </testcase>
    <testcase name="Skips" status="run" result="skipped" classname="Log">
      <skipped message="C:\\src\\LogTests.cpp:20&#x0A;no GPU on this machine"/>
    </testcase>
    <testcase name="DISABLED_Off" status="notrun" result="suppressed" classname="Log" />
  </testsuite>
</testsuites>`;
    const suite = summaries.summarizeGtestXml(xml);
    check('gtest: declared = ran + disabled', suite.declared === 4 && suite.ran === 3 && suite.disabled === 1,
          JSON.stringify(suite));
    check('gtest: the failure with its first message line',
          suite.failed === 1 && suite.failures[0] === 'Log.Fails: C:\\src\\LogTests.cpp:12: Expected equality of these values:',
          JSON.stringify(suite.failures));
    check('gtest: the skip with its reason',
          suite.skipped === 1 && suite.skips[0] === 'Log.Skips: C:\\src\\LogTests.cpp:20: no GPU on this machine',
          JSON.stringify(suite.skips));

    // A filter that matched nothing: gtest exits 0 with tests="0".
    const empty = summaries.summarizeGtestXml('<testsuites tests="0" failures="0" disabled="0" errors="0" name="AllTests"></testsuites>');
    const voidRecord = { suite: 'LoggerAsyncTests', gtest: true, exitCode: 0, timedOut: false, durationSec: 0.1, filter: 'NoSuchSuite.*',
                         xmlPath: 'x.xml', logPath: 'x.log', ...empty };
    check('gtest: a run with no test is void, not passed',
          !summaries.isPassingSuite(voidRecord)
              && summaries.formatSuiteRecord(voidRecord).startsWith('TESTS VOID LoggerAsyncTests: the filter matched no test'),
          summaries.formatSuiteRecord(voidRecord));

    // CTest registrations: one through the run script, one per test case with an
    // environment, a dotnet test and a node gate with their arguments, and suites that
    // are registered but not built.
    const { resolveSuiteRun, suiteArguments } = await import(pathToFileURL(path.join(dist, 'build', 'ctest-registry.js')).href);
    const registrations = { tests: [
        { name: 'EditorTests', command: ['C:/CMake/bin/cmake.exe', '-DTEST_EXE=C:/b/bin/DebugFast/Tests/EditorTests.exe',
                                         '-DTEST_WORKING_DIR=C:/b/bin/DebugFast/Tests', '-P', 'C:/src/cmake/RunTestExecutable.cmake'],
          properties: [{ name: 'WORKING_DIRECTORY', value: 'C:/b/Tests/Editor' }] },
        { name: 'PhysicsTests.Loads', command: ['C:/b/bin/DebugFast/Tests/PhysicsTests.exe', '--gtest_filter=PhysicsTests.Loads'],
          properties: [{ name: 'WORKING_DIRECTORY', value: 'C:/b' },
                       { name: 'ENVIRONMENT', value: ['GE_PHYSICS_BACKEND=jolt', 'GE_NATIVE_DIR=C:/b/bin'] }] },
        { name: 'PhysicsTests.Runs', command: ['C:/b/bin/DebugFast/Tests/PhysicsTests.exe', '--gtest_filter=PhysicsTests.Runs'],
          properties: [{ name: 'WORKING_DIRECTORY', value: 'C:/b' },
                       { name: 'ENVIRONMENT', value: ['GE_PHYSICS_BACKEND=jolt', 'GE_NATIVE_DIR=C:/b/bin'] }] },
        { name: 'ManagedCoreBridgeTests', command: ['C:/dotnet/dotnet.exe', 'test', 'C:/src/Managed/CoreBridgeTests/CoreBridgeTests.csproj', '-c', 'Debug'],
          properties: [{ name: 'WORKING_DIRECTORY', value: 'C:/src' },
                       { name: 'ENVIRONMENT', value: ['GE_NATIVE_DIR=C:/b/bin/DebugFast'] }] },
        { name: 'McpInstrumentSummaries', command: ['C:/node/node.exe', 'C:/src/Tests/Mcp/instrument-summaries-check.mjs'],
          properties: [{ name: 'WORKING_DIRECTORY', value: 'C:/b' }] },
        { name: 'AnimationIKTests_NOT_BUILT', properties: [] },
        { name: 'GPUAnimationTests', properties: [] },
        { name: 'GPUAnimationTests_ClassicDescriptorSets', properties: [{ name: 'ENVIRONMENT', value: ['GE_VK_USE_DESCRIPTOR_BUFFER=0'] }] },
    ] };
    const editor = resolveSuiteRun(registrations, 'EditorTests');
    check('ctest: a script registration runs the executable it names, in its directory',
          editor.kind === 'run' && editor.run.gtest && editor.run.program === 'C:/b/bin/DebugFast/Tests/EditorTests.exe'
              && editor.run.cwd === 'C:/b/bin/DebugFast/Tests',
          JSON.stringify(editor));
    const physics = resolveSuiteRun(registrations, 'PhysicsTests');
    check('ctest: per-case registrations collapse to one whole run with their directory and environment',
          physics.kind === 'run' && physics.run.gtest && physics.run.program === 'C:/b/bin/DebugFast/Tests/PhysicsTests.exe'
              && physics.run.args?.length === 0 && physics.run.cwd === 'C:/b'
              && physics.run.env.GE_PHYSICS_BACKEND === 'jolt' && physics.run.env.GE_NATIVE_DIR === 'C:/b/bin',
          JSON.stringify(physics));
    const oneCase = resolveSuiteRun(registrations, 'PhysicsTests.Loads');
    check('ctest: a per-case registration run by its own name keeps its filter',
          oneCase.kind === 'run' && JSON.stringify(oneCase.run.args) === JSON.stringify(['--gtest_filter=PhysicsTests.Loads']),
          JSON.stringify(oneCase));
    const arguments_ = (run, suite, filter) => (suiteArguments ? suiteArguments(run, suite, 'C:/logs/run.xml', filter) : undefined);
    const editorArgs = editor.kind === 'run' && arguments_(editor.run, 'EditorTests', 'LogQuery.*');
    check('ctest: a gtest suite writes the XML and takes the filter',
          JSON.stringify(editorArgs) === JSON.stringify({ args: ['--gtest_output=xml:C:/logs/run.xml', '--gtest_filter=LogQuery.*'] }),
          JSON.stringify(editorArgs));
    const managedRun = resolveSuiteRun(registrations, 'ManagedCoreBridgeTests');
    const managedArgs = managedRun.kind === 'run' && arguments_(managedRun.run, 'ManagedCoreBridgeTests');
    check('ctest: a dotnet test registration runs as registered, with no gtest flag',
          managedRun.kind === 'run' && managedRun.run.gtest === false && managedRun.run.program === 'C:/dotnet/dotnet.exe'
              && managedRun.run.cwd === 'C:/src' && managedRun.run.env.GE_NATIVE_DIR === 'C:/b/bin/DebugFast'
              && JSON.stringify(managedArgs) === JSON.stringify({ args: ['test', 'C:/src/Managed/CoreBridgeTests/CoreBridgeTests.csproj', '-c', 'Debug'] }),
          JSON.stringify({ managedRun, managedArgs }));
    const nodeRun = resolveSuiteRun(registrations, 'McpInstrumentSummaries');
    const nodeArgs = nodeRun.kind === 'run' && arguments_(nodeRun.run, 'McpInstrumentSummaries');
    check('ctest: a node gate runs as registered, with no gtest flag',
          nodeRun.kind === 'run' && nodeRun.run.gtest === false && nodeRun.run.program === 'C:/node/node.exe' && nodeRun.run.cwd === 'C:/b'
              && JSON.stringify(nodeArgs) === JSON.stringify({ args: ['C:/src/Tests/Mcp/instrument-summaries-check.mjs'] }),
          JSON.stringify({ nodeRun, nodeArgs }));
    const nodeFiltered = nodeRun.kind === 'run' && arguments_(nodeRun.run, 'McpInstrumentSummaries', 'Some.*');
    check('ctest: a filter on a registration that is not a gtest suite is refused',
          typeof nodeFiltered?.refusal === 'string' && /not a gtest suite/.test(nodeFiltered.refusal), JSON.stringify(nodeFiltered));
    const notBuilt = resolveSuiteRun(registrations, 'AnimationIKTests');
    check('ctest: a registered suite that is not built says so and names its target',
          notBuilt.kind === 'notBuilt' && notBuilt.target === 'AnimationIKTests', JSON.stringify(notBuilt));
    const variant = resolveSuiteRun(registrations, 'GPUAnimationTests_ClassicDescriptorSets');
    check("ctest: a registered variant that is not built names its suite's target",
          variant.kind === 'notBuilt' && variant.target === 'GPUAnimationTests', JSON.stringify(variant));
    const unknown = resolveSuiteRun(registrations, 'Physics');
    check('ctest: an unknown suite names the close matches',
          unknown.kind === 'unknown' && unknown.closeMatches.includes('PhysicsTests'), JSON.stringify(unknown));

    // A registration that is not a gtest suite reads as its exit code and its last lines.
    const longOutput = Array.from({ length: 400 }, (_, i) => `line ${i} ${'x'.repeat(60)}`).join('\n') + '\nPassed!  - Failed: 0, Passed: 45';
    const scriptRecord = { suite: 'ManagedCoreBridgeTests', gtest: false, exitCode: 0, timedOut: false, durationSec: 9.5,
                           failures: [], skips: [], outputTail: summaries.outputTail?.(longOutput), logPath: 'C:/logs/m.log' };
    const scriptText = summaries.formatSuiteRecord(scriptRecord);
    check('script run: passed by exit code, with the last lines and the log, under 2 KB',
          summaries.isPassingSuite(scriptRecord)
              && scriptText.startsWith('TESTS PASSED ManagedCoreBridgeTests: exit 0 in 9.5 s; not a gtest suite, so no counts')
              && scriptText.includes('  Passed!  - Failed: 0, Passed: 45\nlog: C:/logs/m.log') && Buffer.byteLength(scriptText) < 2048,
          `${Buffer.byteLength(scriptText)} bytes: ${scriptText.slice(0, 300)}`);
    check('script run: a nonzero exit fails', !summaries.isPassingSuite({ ...scriptRecord, exitCode: 1 }), 'exit 1 passed');

    // The registered environment reaches the process: a variable is set, and on Windows
    // a registered PATH replaces the inherited one instead of sitting beside it.
    const { runToLog } = await import(pathToFileURL(path.join(dist, 'build', 'process.js')).href);
    const envLog = path.join(os.tmpdir(), `instrument-summaries-env-${process.pid}.log`);
    const probeDir = path.join(os.tmpdir(), 'instrument-summaries-probe-bin');
    await runToLog(process.execPath,
                   ['-e', 'const keys = Object.keys(process.env).filter((k) => k.toLowerCase() === "path");'
                          + 'console.log(JSON.stringify({ probe: process.env.GE_PROBE, keys, path: process.env[keys[0]] }))'],
                   { cwd: os.tmpdir(), logPath: envLog, timeoutMs: 30_000, env: { GE_PROBE: '1', PATH: probeDir } });
    const seen = JSON.parse(fs.readFileSync(envLog, 'utf8').trim());
    fs.rmSync(envLog, { force: true });
    check('process: the registered environment reaches the process, with one PATH',
          seen.probe === '1' && seen.keys.length === 1 && seen.path === probeDir, JSON.stringify(seen));

    // A build log: 40 distinct errors, each printed twice the way MSBuild repeats them, and warnings.
    const log = [];
    for (let i = 0; i < 40; i++) {
        const error = `C:\\src\\File${i}.cpp(${i},5): error C2065: 'name${i}': undeclared identifier`;
        log.push(error, `${error} [C:\\build\\Engine.vcxproj]`);
    }
    for (let i = 0; i < 7; i++) log.push(`C:\\src\\Warn.cpp(${i}): warning C4100: unreferenced parameter`);
    log.push('Compiling ErrorReporter.cpp');
    const build = summaries.summarizeBuildOutput(log.join('\r\n'), false);
    check('build: unique errors counted, the first ten listed', build.errors === 40 && build.errorLines.length === 10,
          JSON.stringify({ errors: build.errors, listed: build.errorLines.length }));
    check('build: warnings counted, not listed', build.warnings === 7 && build.warningLines === undefined,
          JSON.stringify({ warnings: build.warnings, lines: build.warningLines }));
    const text = summaries.formatBuildRecord({
        ...build, exitCode: 1, timedOut: false, durationSec: 12.5, targets: ['Engine'], preset: 'vs2026-x64-local',
        config: 'DebugFast', binaries: [], binaryCount: 0, logPath: 'C:/Dev/build/ToolLogs/build-Engine.log',
    });
    check('build: the summary of a failing build stays under 2 KB', Buffer.byteLength(text) < 2048,
          `${Buffer.byteLength(text)} bytes`);

    // The descriptions and the CLI overview carry the guidance.
    const getLog = tool('get_log').description;
    check('get_log description: a level, a pattern, since, no polling, not after a build',
          /pattern/.test(getLog) && /since=<nextSince>/.test(getLog) && /Make that call/.test(getLog)
              && /Do not poll/.test(getLog)
              && /build_engine, run_tests/.test(getLog) && /warning level/.test(getLog),
          getLog);
    for (const name of ['build_engine', 'run_tests']) {
        const description = tool(name).description;
        check(`${name} description: the summary is the record; no get_log after it`,
              /is the record|are the record/.test(description) && /do not call get_log/.test(description), description);
    }
    for (const name of ['take_screenshot', 'trigger_capture']) {
        check(`${name} description: the record line`, /record line/.test(tool(name).description), tool(name).description);
    }
    const { overviewHelp } = await import(pathToFileURL(path.join(dist, 'cli', 'help.js')).href);
    check('CLI overview: the reading-results guidance',
          /Reading results:[\s\S]*--since[\s\S]*paste the next line/.test(overviewHelp()),
          'overviewHelp() has no Reading results block naming --since and the next line');
} finally {
    fake.close();
}

if (failures.length > 0) {
    console.error(`instrument-summaries-check: ${failures.length} arm(s) failed:\n  ${failures.join('\n  ')}`);
    process.exit(1);
}
console.log('instrument-summaries-check: every arm held');
process.exit(0);
