// Each field the stub reflects is a field the engine reflects. The committed C# mirrors are the
// engine's fields as the scanner reads the headers (ApiDescriptionDriftTests keeps them current);
// a mirror's member is the reflected name with its first letter in upper case.

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';
import { StubAbi } from '../dist/test/stub-abi.js';

const kMirrorDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../../../Managed/Scripting.Bindings/Generated/Components');

/** Every mirrored component's public member names, by component name. */
function mirrorMembers() {
    const members = new Map();
    const files = fs.readdirSync(kMirrorDir, { recursive: true }).filter((f) => f.endsWith('.g.cs'));
    for (const file of files) {
        const text = fs.readFileSync(path.join(kMirrorDir, file), 'utf8');
        for (const block of text.split('[StructLayout').slice(1)) {
            const component = /partial struct (\w+) : IComponent/.exec(block);
            if (!component) continue;
            const names = [...block.matchAll(/^\s+public (?:readonly )?(?!struct |enum )[\w.<>?]+ (\w+)\s*;?\s*$/gm)].map((match) => match[1]);
            members.set(component[1], new Set(names));
        }
    }
    return members;
}

test('each field the stub reflects is a field of the engine component it stands in for', () => {
    const mirrors = mirrorMembers();
    const stub = JSON.parse(new StubAbi().reflectionJson());
    for (const component of stub.components) {
        const engineFields = mirrors.get(component.name);
        assert.ok(engineFields, `${component.name} has no C# mirror under ${kMirrorDir}`);
        for (const field of component.fields) {
            const member = field.name[0].toUpperCase() + field.name.slice(1);
            assert.ok(engineFields.has(member),
                `test/stub-abi.ts reflects ${component.name}.${field.name}, which the engine does not; ` +
                `its fields are ${[...engineFields].join(', ')}. Name the field as the component's header does.`);
        }
    }
});
