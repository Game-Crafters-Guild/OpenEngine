// `ge batch` ends with its own exit code: 0 when stdin is a file and the editor answers, 3 when
// no editor listens, 4 when a held pipe waits on a silent editor. `--help` lists only the flags
// batch reads, and `--dry-run` exits with 2; neither connects.
//
// The editor is a fake TCP server speaking the debug protocol; no editor binary.
// The CLI runs through mcp/ge.mjs, which brings mcp/dist up to date first.
//
// Exit 0 = every arm held; 1 = a failure; 2 = the harness could not run.
//
//   node Tests/Mcp/batch-stdin-check.mjs

import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const geEntry = path.join(repoRoot, 'mcp', 'ge.mjs');

// Below the ctest TIMEOUT, so a first build of mcp/dist that runs long fails here with a reason.
const kTotalBudgetMs = 540_000;
const deadline = Date.now() + kTotalBudgetMs;
const kExitUsage = 2;
const kExitUnreachable = 3;
const kExitTimeout = 4;
// The watchdog fires at kWatchdogMs; a process still alive at kHeldPipeCapMs hangs.
const kWatchdogMs = 2_000;
const kHeldPipeCapMs = 20_000;

const failures = [];
function check(label, condition, detail) {
    if (!condition) failures.push(`${label}\n    ${detail}`);
}

let connections = 0;
const fake = net.createServer((socket) => {
    connections++;
    let buffer = '';
    socket.on('data', (chunk) => {
        buffer += chunk.toString();
        const lines = buffer.split('\n');
        buffer = lines.pop() ?? '';
        for (const line of lines) {
            if (!line.trim()) continue;
            const request = JSON.parse(line);
            socket.write(JSON.stringify({ id: request.id, ok: true, result: { answered: request.method } }) + '\n');
        }
    });
    socket.on('error', () => { /* the client hangs up at the end */ });
});

// An editor that accepts the connection and never answers.
const silent = net.createServer((socket) => socket.on('error', () => { /* the client hangs up */ }));

// A port that was free a moment ago and has no listener now.
async function closedPort() {
    const probe = net.createServer();
    await new Promise((resolve) => probe.listen(0, '127.0.0.1', resolve));
    const { port } = probe.address();
    await new Promise((resolve) => probe.close(resolve));
    return port;
}

// stdin is { file } for a file, { held } for text written to a pipe that stays open, or null.
// A run past capMs is killed and resolves with code null.
function runBatch(args, stdin, capMs = Infinity) {
    return new Promise((resolve, reject) => {
        const remaining = deadline - Date.now();
        if (remaining <= 0) {
            reject(new Error(`ran out of the ${kTotalBudgetMs}ms budget before: ge batch ${args.join(' ')}`));
            return;
        }
        const fd = stdin?.file ? fs.openSync(stdin.file, 'r') : null;
        const stdinMode = fd ?? (stdin?.held ? 'pipe' : 'ignore');
        const child = spawn(process.execPath, [geEntry, 'batch', ...args],
                            { cwd: repoRoot, stdio: [stdinMode, 'pipe', 'pipe'] });
        if (fd !== null) fs.closeSync(fd);
        if (stdin?.held) {
            child.stdin.on('error', () => { /* the child exits with the pipe still open */ });
            child.stdin.write(stdin.held);
        }
        let stdout = '';
        let stderr = '';
        child.stdout.on('data', (d) => { stdout += d.toString(); });
        child.stderr.on('data', (d) => { stderr += d.toString(); });
        const capTimer = Number.isFinite(capMs) ? setTimeout(() => child.kill(), capMs) : null;
        const timer = setTimeout(() => {
            child.kill();
            reject(new Error(`timed out: ge batch ${args.join(' ')}`));
        }, remaining);
        child.on('close', () => clearTimeout(capTimer));
        child.on('error', (e) => { clearTimeout(timer); reject(e); });
        child.on('close', (code) => { clearTimeout(timer); resolve({ code, stdout, stderr }); });
    });
}

const scratch = fs.mkdtempSync(path.join(os.tmpdir(), 'ge-batch-stdin-'));
const requests = path.join(scratch, 'requests.ndjson');
fs.writeFileSync(requests, '{"method":"get_editor_state"}\n{"method":"get_camera"}\n');

await new Promise((resolve) => fake.listen(0, '127.0.0.1', resolve));
await new Promise((resolve) => silent.listen(0, '127.0.0.1', resolve));
try {
    const answered = await runBatch(['--port', String(fake.address().port)], { file: requests });
    check('stdin from a file, editor answers: exit 0 with both responses',
          answered.code === 0 && answered.stdout.includes('"answered":"get_editor_state"') &&
              answered.stdout.includes('"answered":"get_camera"') && !answered.stderr.includes('TypeError'),
          `exit ${answered.code}, stdout: ${answered.stdout.trim()}, stderr: ${answered.stderr.trim()}`);

    const unreachable = await runBatch(['--port', String(await closedPort())], { file: requests });
    check('stdin from a file, no editor: exit 3 with no TypeError',
          unreachable.code === kExitUnreachable && !unreachable.stderr.includes('TypeError'),
          `exit ${unreachable.code}, stderr: ${unreachable.stderr.trim()}`);

    const held = await runBatch(['--port', String(silent.address().port), '--timeout', String(kWatchdogMs)],
                                { held: '{"method":"get_camera"}\n' }, kHeldPipeCapMs);
    check('stdin from a pipe held open, editor silent: exit 4 with no TypeError',
          held.code === kExitTimeout && !held.stderr.includes('TypeError'),
          `exit ${held.code ?? `none, killed after ${kHeldPipeCapMs}ms`}, stderr: ${held.stderr.trim()}`);

    const connectionsBefore = connections;
    const help = await runBatch(['--port', String(fake.address().port), '--help'], null);
    const listsOnlyBatchFlags = ['--port', '--host', '--timeout', '--quiet'].every((f) => help.stdout.includes(f)) &&
                                !help.stdout.includes('--dry-run') && !help.stdout.includes('--json');
    check('ge batch --help prints the batch help with only --port, --host, --timeout and --quiet, exits 0 and does not connect',
          help.code === 0 && help.stdout.includes('ge batch < requests.ndjson') && listsOnlyBatchFlags &&
              connections === connectionsBefore,
          `exit ${help.code}, connections ${connections - connectionsBefore}, stdout: ${help.stdout.trim()}`);

    const dryRun = await runBatch(['--port', String(fake.address().port), '--dry-run'], { file: requests });
    check('ge batch --dry-run exits 2 with a message and does not connect',
          dryRun.code === kExitUsage && dryRun.stderr.includes('--dry-run') && connections === connectionsBefore,
          `exit ${dryRun.code}, connections ${connections - connectionsBefore}, stderr: ${dryRun.stderr.trim()}`);
} catch (e) {
    console.error(`harness could not run: ${e.message}`);
    fake.close();
    silent.close();
    fs.rmSync(scratch, { recursive: true, force: true });
    process.exit(2);
}
fake.close();
silent.close();
fs.rmSync(scratch, { recursive: true, force: true });

if (failures.length) {
    console.error(`FAIL: ${failures.length} arm(s)\n  ${failures.join('\n  ')}`);
    process.exit(1);
}
console.log('PASS: ge batch with stdin from a file or a held pipe ends with its own exit code; --help and --dry-run do not connect');
