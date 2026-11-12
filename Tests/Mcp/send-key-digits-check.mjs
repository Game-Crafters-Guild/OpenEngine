// send_key reads an integer from 0 to 9 as the digit key, so `ge send_key --key 8`
// sends the 8 key as the help says. GLFW key codes run from 32 to 348: an integer in that range
// stays a raw key code, and any other integer is refused before a request is sent.
//
// The editor is a fake TCP server speaking the debug protocol; no editor binary.
// The CLI runs through mcp/ge.mjs, which brings mcp/dist up to date first.
//
// Exit 0 = every arm held; 1 = a failure; 2 = the harness could not run.
//
//   node Tests/Mcp/send-key-digits-check.mjs

import { spawn } from 'node:child_process';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const geEntry = path.join(repoRoot, 'mcp', 'ge.mjs');

// Below the ctest TIMEOUT, so a first build of mcp/dist that runs long fails here with a reason.
const kTotalBudgetMs = 540_000;
const deadline = Date.now() + kTotalBudgetMs;

const failures = [];
function check(label, condition, detail) {
    if (!condition) failures.push(`${label}\n    ${detail}`);
}

const received = [];
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
            socket.write(JSON.stringify({ id: request.id, ok: true, result: { sent: true } }) + '\n');
        }
    });
    socket.on('error', () => { /* the client hangs up at the end */ });
});

function runCli(args) {
    return new Promise((resolve, reject) => {
        const remaining = deadline - Date.now();
        if (remaining <= 0) {
            reject(new Error(`ran out of the ${kTotalBudgetMs}ms budget before: ge ${args.join(' ')}`));
            return;
        }
        const child = spawn(process.execPath, [geEntry, ...args, '--port', String(fake.address().port), '--json'],
                            { cwd: repoRoot, stdio: ['ignore', 'pipe', 'pipe'] });
        let stdout = '';
        let stderr = '';
        child.stdout.on('data', (d) => { stdout += d.toString(); });
        child.stderr.on('data', (d) => { stderr += d.toString(); });
        const timer = setTimeout(() => {
            child.kill();
            reject(new Error(`timed out: ge ${args.join(' ')}`));
        }, remaining);
        child.on('error', (e) => { clearTimeout(timer); reject(e); });
        child.on('close', (code) => { clearTimeout(timer); resolve({ code, stdout, stderr }); });
    });
}

async function sentKey(token) {
    received.length = 0;
    const { code, stderr } = await runCli(['send_key', '--key', token]);
    const request = received.find((r) => r.method === 'send_key');
    if (code !== 0 || !request) throw new Error(`ge send_key --key ${token} exit ${code}\n${stderr}`);
    return request.params.key;
}

async function refusedKey(token) {
    received.length = 0;
    // With --json, the CLI prints its error object on stdout.
    const { code, stdout } = await runCli(['send_key', '--key', token]);
    return { code, stdout, sent: received.some((r) => r.method === 'send_key') };
}

await new Promise((resolve) => fake.listen(0, '127.0.0.1', resolve));
try {
    const digit = await sentKey('8');
    check('--key 8 sends the digit key', digit === '8', `key: ${JSON.stringify(digit)}`);
    const raw = await sentKey('56');
    check('--key 56 stays the raw key code', raw === 56, `key: ${JSON.stringify(raw)}`);
    const refused = await refusedKey('12');
    check('--key 12 is refused with the fix and sends nothing',
          refused.code !== 0 && !refused.sent &&
              refused.stdout.includes('key 12 names no key: key takes a key name, a digit 0 to 9, or a GLFW key code from 32 to 348'),
          `exit ${refused.code}, sent ${refused.sent}, stdout: ${refused.stdout.trim()}`);
    const pastLast = await refusedKey('349');
    check('--key 349 is refused with the fix and sends nothing',
          pastLast.code !== 0 && !pastLast.sent &&
              pastLast.stdout.includes('key 349 names no key: key takes a key name, a digit 0 to 9, or a GLFW key code from 32 to 348'),
          `exit ${pastLast.code}, sent ${pastLast.sent}, stdout: ${pastLast.stdout.trim()}`);
} catch (e) {
    console.error(`harness could not run: ${e.message}`);
    fake.close();
    process.exit(2);
}
fake.close();

if (failures.length) {
    console.error(`FAIL: ${failures.length} arm(s)\n  ${failures.join('\n  ')}`);
    process.exit(1);
}
console.log('PASS: send_key reads 0 to 9 as the digit key, keeps a raw key code and refuses 10 to 31 and 349 up');
