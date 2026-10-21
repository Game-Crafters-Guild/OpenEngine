// Engine.create's choices and the frame loop: the build by cross-origin isolation, the
// WebGPU and release checks, the hidden-tab timer, resize tracking and onFrame.

import assert from 'node:assert/strict';
import test from 'node:test';
import { createEngine, selectBuild } from '../dist/src/engine.js';
import { fingerprint } from '../dist/src/reflection.js';
import { StubAbi } from '../dist/test/stub-abi.js';
import { FakeHost, fakeCanvas, startEngine } from '../dist/test/harness.js';

test("threads: 'auto' takes the threaded build only on a cross-origin isolated page, and says why", async () => {
    assert.deepEqual(selectBuild('auto', true), { build: 'mt', reason: 'the page is cross-origin isolated' });
    assert.equal(selectBuild('auto', false).build, 'st');
    assert.equal(selectBuild('single', true).build, 'st');
    assert.throws(() => selectBuild('multi', false), { code: 'InvalidArgument', message: /coi-serviceworker.js/ });
    const host = new FakeHost();
    host.isolated = true;
    let loaded = null;
    await createEngine({ canvas: fakeCanvas() }, host, async (build) => { loaded = build; return new StubAbi(); }, { version: '0.1.0', fingerprint: null });
    assert.equal(loaded, 'mt');
    assert.match(host.logs[0], /using the threaded build because the page is cross-origin isolated/);
});

test('Engine.create rejects without WebGPU, naming the browsers', async () => {
    const host = new FakeHost();
    host.webGpu = false;
    await assert.rejects(createEngine({ canvas: fakeCanvas() }, host, async () => new StubAbi(), { version: '0.1.0', fingerprint: null }),
        { code: 'NoWebGPU', message: /no WebGPU. Use a current Chrome or Edge/ });
});

test('a library built against another release rejects naming both versions and the fix', async () => {
    const abi = new StubAbi();
    abi.engineVersion = '0.2.0';
    const load = async () => abi;
    await assert.rejects(createEngine({ canvas: fakeCanvas() }, new FakeHost(), load, { version: '0.1.0', fingerprint: '0123456789abcdef' }),
        { code: 'VersionMismatch', message: /engine module is release 0.2.0 and this library is 0.1.0.*Update @game-crafters-guild\/openengine-web to 0.2.0/ });
    const matching = await createEngine({ canvas: fakeCanvas() }, new FakeHost(), async () => new StubAbi(),
        { version: '0.1.0', fingerprint: fingerprint(new StubAbi().reflectionJson()) });
    assert.ok(matching.scene.camera);
});

test('a hidden tab ticks on a 33 ms timer, a visible one on animation frames', async () => {
    const { abi, host, engine } = await startEngine();
    host.hidden = true;
    engine.run();
    assert.equal(host.frames.size, 0);
    assert.deepEqual([...host.timers.values()].map((t) => t.delayMs), [33]);
    host.fireTimers();
    assert.equal(abi.calls.filter((c) => c === 'ge_tick').length, 1);
    host.setHidden(false);
    assert.equal(host.timers.size, 0);
    assert.equal(host.frames.size, 1);
    engine.pause();
    assert.equal(host.frames.size, 0);
});

test('a canvas resize reaches the engine as CSS size and device pixel ratio', async () => {
    const { abi, host, canvas } = await startEngine();
    assert.deepEqual(abi.lastResize, [800, 600, 2]);
    canvas.clientWidth = 1024;
    host.dpr = 1.5;
    host.resizeCanvas();
    assert.deepEqual(abi.lastResize, [1024, 600, 1.5]);
});

test('a phone draws within a megapixel by default and a page can raise or lift the cap', async () => {
    const phone = () => {
        const host = new FakeHost();
        host.mobile = true;
        host.dpr = 2.625;
        return host;
    };
    const phoneCanvas = () => Object.assign(fakeCanvas(), { clientWidth: 411, clientHeight: 785 });
    const capped = await startEngine({ canvas: phoneCanvas() }, undefined, phone());
    const [width, height, ratio] = capped.abi.lastResize;
    assert.deepEqual([width, height], [411, 785]);
    assert.ok(ratio < 2.625, `the ratio ${ratio} was not lowered`);
    assert.ok(Math.abs(width * height * ratio * ratio - 1_000_000) < 1, `the buffer is ${width * height * ratio * ratio} pixels`);

    const raised = await startEngine({ canvas: phoneCanvas(), maxDrawingBufferPixels: 1_500_000 }, undefined, phone());
    assert.ok(Math.abs(411 * 785 * raised.abi.lastResize[2] ** 2 - 1_500_000) < 1);
    const lifted = await startEngine({ canvas: phoneCanvas(), maxDrawingBufferPixels: Infinity }, undefined, phone());
    assert.equal(lifted.abi.lastResize[2], 2.625);

    const desktop = await startEngine({ canvas: Object.assign(fakeCanvas(), { clientWidth: 2560, clientHeight: 1440 }) });
    assert.equal(desktop.abi.lastResize[2], 2, 'a desktop page is not capped by default');
});

test('a hidden canvas is refused before the engine loads, naming the cause and the fix', async () => {
    for (const size of [{ clientWidth: 0, clientHeight: 0 }, { clientWidth: 800, clientHeight: 0 }]) {
        let loaded = false;
        const canvas = Object.assign(fakeCanvas(), size);
        await assert.rejects(createEngine({ canvas }, new FakeHost(), async () => { loaded = true; return new StubAbi(); },
                                          { version: '0.1.0', fingerprint: null }),
            { code: 'InvalidArgument', message: /it is hidden \(display: none, or inside a hidden element\).*Show it before Engine.create/ });
        assert.equal(loaded, false, 'the engine module was loaded for a hidden canvas');
    }
});

test('a create that fails leaves no resize observer and no engine running', async () => {
    const host = new FakeHost();
    const abi = new StubAbi();
    abi.refuseResize = true;
    await assert.rejects(createEngine({ canvas: fakeCanvas() }, host, async () => abi, { version: '0.1.0', fingerprint: null }));
    assert.equal(host.resizeListenerCount(), 0);
    assert.ok(abi.calls.includes('ge_shutdown'), 'the engine was not shut down');
});

test('a drawing-buffer cap that is not a positive number or Infinity is refused, naming the option', async () => {
    for (const cap of [0, -5, NaN, 'big']) {
        await assert.rejects(startEngine({ canvas: fakeCanvas(), maxDrawingBufferPixels: cap }),
            { code: 'InvalidArgument', message: /maxDrawingBufferPixels takes a number of pixels above 0, or Infinity/ },
            `the cap ${String(cap)}`);
    }
});

test('onFrame callbacks run before the tick with the step in seconds, until unsubscribed', async () => {
    const { abi, host, engine } = await startEngine();
    const steps = [];
    const stop = engine.onFrame((dt) => steps.push([dt, abi.calls.filter((c) => c === 'ge_tick').length]));
    engine.run();
    host.fireFrames(1000);
    host.fireFrames(1016);
    stop();
    host.fireFrames(1032);
    assert.deepEqual(steps.map(([, ticks]) => ticks), [0, 1]);
    assert.equal(steps[0][0], 0);
    assert.ok(Math.abs(steps[1][0] - 0.016) < 1e-12);
    assert.equal(abi.calls.filter((c) => c === 'ge_tick').length, 3);
});
