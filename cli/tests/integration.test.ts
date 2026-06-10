// Optional integration test — runs end-to-end against a real local Unreal
// project ONLY when UNREAL_CLI_INTEGRATION is set. It is NOT part of the
// default `vitest run` gate (acceptance criterion: "gated behind an env
// flag, not required for the default test run").
//
//   UNREAL_CLI_INTEGRATION=1 \
//   UNREAL_CLI_INTEGRATION_PROJECT=/abs/path/to/SomeProject \
//   npm test
//
// Without UNREAL_CLI_INTEGRATION_PROJECT it scaffolds a throwaway project
// in a temp dir, configures it, and probes status offline — no editor
// binary required.

import { describe, it, expect, afterEach } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { createProject } from '../src/lib/create-project.js';
import { configure } from '../src/lib/configure.js';
import { getStatus } from '../src/lib/status.js';
import { makeTempDir, rmTempDir } from './helpers.js';

const RUN = process.env['UNREAL_CLI_INTEGRATION'] === '1' || process.env['UNREAL_CLI_INTEGRATION'] === 'true';

const dirs: string[] = [];
afterEach(() => {
  while (dirs.length) rmTempDir(dirs.pop()!);
});

describe.skipIf(!RUN)('integration (env-gated)', () => {
  it('scaffolds, configures, and reports status for a real-ish project', async () => {
    const explicit = process.env['UNREAL_CLI_INTEGRATION_PROJECT'];
    let projectDir: string;
    if (explicit) {
      projectDir = explicit;
    } else {
      const parent = makeTempDir();
      dirs.push(parent);
      projectDir = path.join(parent, 'IntegrationGame');
      const created = await createProject({ projectDir, engineAssociation: '5.7' });
      expect(created.kind).toBe('success');
    }

    const cfg = await configure({ projectDir, host: 'http://localhost:5220', token: 'integ' });
    expect(cfg.kind).toBe('success');
    expect(fs.existsSync(path.join(projectDir, '.env'))).toBe(true);

    const status = await getStatus({ projectDir, noProbe: true });
    expect(status.project?.engineAssociation).toBe('5.7');
    expect(status.connection.url).toContain('localhost:5220');
  });
});
