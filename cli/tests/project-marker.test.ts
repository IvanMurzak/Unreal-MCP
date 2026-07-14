import { describe, it, expect, beforeEach, afterEach } from 'vitest';
import * as fs from 'fs';
import * as os from 'os';
import * as path from 'path';
import {
  readProjectMarker,
  writeProjectMarker,
  serializeProjectMarker,
  upsertServerTarget,
  projectMarkerPath,
  PROJECT_MARKER_DIR,
  PROJECT_MARKER_FILE,
} from '../src/utils/project-marker.js';

describe('project-marker', () => {
  let dir: string;

  beforeEach(() => {
    dir = fs.mkdtempSync(path.join(os.tmpdir(), 'unreal-mcp-marker-'));
  });
  afterEach(() => {
    fs.rmSync(dir, { recursive: true, force: true });
  });

  it('reads null when no marker exists', () => {
    expect(readProjectMarker(dir)).toBeNull();
  });

  it('writes camelCase, 2-space-indented JSON at .ai-game-dev/project.json (C# byte-compat)', () => {
    const written = writeProjectMarker(dir, { serverTarget: 'https://ai-game.dev', portOverride: 24567 });
    expect(written).toBe(path.join(dir, PROJECT_MARKER_DIR, PROJECT_MARKER_FILE));
    const raw = fs.readFileSync(written, 'utf-8');
    expect(raw).toBe('{\n  "serverTarget": "https://ai-game.dev",\n  "portOverride": 24567\n}');
  });

  it('omits null/blank fields on serialize', () => {
    expect(serializeProjectMarker({ serverTarget: 'https://ai-game.dev' })).toBe(
      '{\n  "serverTarget": "https://ai-game.dev"\n}',
    );
    expect(serializeProjectMarker({})).toBe('{}');
    expect(serializeProjectMarker({ serverTarget: '' })).toBe('{}');
  });

  it('round-trips serverTarget + portOverride', () => {
    writeProjectMarker(dir, { serverTarget: 'http://localhost:24567', portOverride: 24567 });
    const marker = readProjectMarker(dir);
    expect(marker).toEqual({ serverTarget: 'http://localhost:24567', portOverride: 24567 });
  });

  it('tolerates a blank marker file (→ {}) and a garbage marker (→ {})', () => {
    const p = projectMarkerPath(dir);
    fs.mkdirSync(path.dirname(p), { recursive: true });
    fs.writeFileSync(p, '   ');
    expect(readProjectMarker(dir)).toEqual({});
    fs.writeFileSync(p, 'not json {{{');
    expect(readProjectMarker(dir)).toEqual({});
  });

  it('ignores unknown fields on read (forwards-compatible)', () => {
    const p = projectMarkerPath(dir);
    fs.mkdirSync(path.dirname(p), { recursive: true });
    fs.writeFileSync(p, JSON.stringify({ serverTarget: 'https://ai-game.dev', futureField: 42 }));
    expect(readProjectMarker(dir)).toEqual({ serverTarget: 'https://ai-game.dev' });
  });

  it('tolerates a UTF-8 BOM', () => {
    const p = projectMarkerPath(dir);
    fs.mkdirSync(path.dirname(p), { recursive: true });
    fs.writeFileSync(p, '﻿' + JSON.stringify({ serverTarget: 'https://ai-game.dev' }));
    expect(readProjectMarker(dir)).toEqual({ serverTarget: 'https://ai-game.dev' });
  });

  describe('upsertServerTarget', () => {
    it('records the server target, preserving an existing portOverride', () => {
      writeProjectMarker(dir, { portOverride: 25001 });
      const written = upsertServerTarget(dir, 'https://ai-game.dev');
      expect(written).toBe(projectMarkerPath(dir));
      expect(readProjectMarker(dir)).toEqual({ serverTarget: 'https://ai-game.dev', portOverride: 25001 });
    });

    it('creates the marker when none exists', () => {
      upsertServerTarget(dir, 'http://localhost:8080');
      expect(readProjectMarker(dir)).toEqual({ serverTarget: 'http://localhost:8080' });
    });

    it('is a no-op for a blank/undefined target', () => {
      expect(upsertServerTarget(dir, '')).toBeNull();
      expect(upsertServerTarget(dir, undefined)).toBeNull();
      expect(readProjectMarker(dir)).toBeNull();
    });

    it('trims the target before writing', () => {
      upsertServerTarget(dir, '  https://ai-game.dev  ');
      expect(readProjectMarker(dir)).toEqual({ serverTarget: 'https://ai-game.dev' });
    });
  });
});
