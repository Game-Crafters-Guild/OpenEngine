#!/usr/bin/env node
// Run against an isolated Editor launched with --project and a verified-free port:
//   GE_EDITOR_DEBUG_PORT=<port> node Tests/EditorHarness/project-startup.mjs --scene=<path> --parent=<path>
// The fixture project names the scene in .Editor/ProjectSettings.json. Preferences
// under GE_EDITOR_USER_DATA_ROOT set newProjectLocation to the expected parent and
// lastBrowsedDirectory to a different existing folder. Cases: command-line startup
// opens that scene without a save prompt; Import uses the remembered parent.
import assert from 'node:assert/strict';
import path from 'node:path';
import { parseArgs } from 'node:util';
import { ipc, sleep, PORT } from './lib/ipc.mjs';

const { values } = parseArgs({ options: { scene: { type: 'string' }, parent: { type: 'string' } } });
assert.ok(values.scene && values.parent, '--scene and --parent are required');

async function call(method, params = {}) {
    const result = await ipc(method, params);
    assert.ok(!result?.error, `${method}: ${JSON.stringify(result)}`);
    return result;
}

async function until(label, read, accept) {
    const deadline = Date.now() + 30000;
    do {
        const value = await read();
        if (accept(value)) return value;
        await sleep(100);
    } while (Date.now() < deadline);
    throw new Error(`Timed out: ${label}`);
}

function containsText(node, expected) {
    return node.text === expected || (node.ch ?? []).some(child => containsText(child, expected));
}

async function visibleElement(elementId) {
    return until(`${elementId} visible`,
        () => call('get_ui_tree', { rootId: elementId, includeHidden: true, maxDepth: 8, maxTextLength: 0 }),
        node => !node.hidden && node.w > 0);
}

try {
    console.log(`Project startup regression on port ${PORT}`);
    const state = await until('startup scene opens', () => call('get_editor_state'), value => Boolean(value.scenePath));
    assert.equal(path.resolve(state.scenePath), path.resolve(values.scene), JSON.stringify(state));
    assert.ok(!state.sceneLoadError, JSON.stringify(state));
    assert.ok(!state.modal?.open, JSON.stringify(state));
    console.log('PASS command-line project opens its scene without a save prompt');

    await call('click_element', { elementId: 'TopToolbarLeft2' });
    await visibleElement('recent-project-card-open');
    await call('click_element', { elementId: 'recent-project-card-open' });
    await visibleElement('import-archive-tab');
    await call('click_element', { elementId: 'import-archive-tab' });
    const field = await visibleElement('import-destination-field');
    assert.ok(containsText(field, path.resolve(values.parent).replaceAll('\\', '/')), JSON.stringify(field));
    console.log('PASS Import destination uses the remembered parent');
} catch (error) {
    console.error(error.stack ?? error);
    process.exitCode = 1;
}
