import { describe, it, expect } from 'vitest';
import * as fs from 'fs';
import * as path from 'path';
import { fileURLToPath } from 'url';
import {
  EXTENSIONS_CATALOG,
  EXTENSION_CATALOG_SCHEMA_VERSION,
  type ExtensionDescriptor,
} from '../src/utils/extensions-catalog.js';

// The CLI's EXTENSIONS_CATALOG mirror MUST stay value-equivalent to the SHARED source of
// truth `cli/extensions.catalog.json` (the future in-editor extensions panel / app reads the
// same JSON). If the catalog gains/changes an entry, this test fails until the mirror is
// updated — the drift tripwire that keeps the CLI + panel + app from diverging. Same
// discipline as godot-cli's `extensions-catalog-parity.test.ts`.
const __dirname = path.dirname(fileURLToPath(import.meta.url));
const CATALOG_JSON = path.resolve(__dirname, '..', 'extensions.catalog.json');

interface CatalogJsonTool {
  name?: string;
  description?: string;
}

interface CatalogJsonEntry {
  extensionId?: string;
  name?: string;
  description?: string;
  pluginName?: string;
  repo?: string | null;
  version?: string | null;
  minCoreVersion?: string | null;
  enginePlugins?: string[];
  tools?: CatalogJsonTool[];
}

/** Trim a string-or-null-or-absent field to a non-empty string, else null (mirrors the descriptor's nullable fields). */
function trimOrNull(v: string | null | undefined): string | null {
  if (v === undefined || v === null) return null;
  const s = String(v).trim();
  return s === '' ? null : s;
}

/** Normalize a raw JSON entry to the canonical ExtensionDescriptor shape. */
function normalizeJsonEntry(e: CatalogJsonEntry): ExtensionDescriptor {
  return {
    extensionId: (e.extensionId ?? '').trim(),
    name: (e.name ?? '').trim(),
    description: (e.description ?? '').trim(),
    pluginName: (e.pluginName ?? '').trim(),
    repo: trimOrNull(e.repo),
    version: trimOrNull(e.version),
    minCoreVersion: trimOrNull(e.minCoreVersion),
    enginePlugins: (e.enginePlugins ?? []).map((p) => p.trim()).filter((p) => p !== ''),
    tools: (e.tools ?? [])
      .filter((t) => t.name && t.name.trim() !== '')
      .map((t) => ({ name: (t.name ?? '').trim(), description: (t.description ?? '').trim() })),
  };
}

describe('extensions-catalog parity with cli/extensions.catalog.json', () => {
  it('the catalog JSON is reachable from the test (relative layout sanity)', () => {
    expect(fs.existsSync(CATALOG_JSON)).toBe(true);
  });

  it('the JSON schemaVersion matches the TS mirror constant', () => {
    const raw = JSON.parse(fs.readFileSync(CATALOG_JSON, 'utf-8')) as { schemaVersion?: number };
    expect(raw.schemaVersion).toBe(EXTENSION_CATALOG_SCHEMA_VERSION);
  });

  it('the TS mirror EXTENSIONS_CATALOG matches the JSON source of truth exactly', () => {
    const raw = JSON.parse(fs.readFileSync(CATALOG_JSON, 'utf-8')) as {
      extensions?: CatalogJsonEntry[];
    };
    const expected = (raw.extensions ?? [])
      .filter(
        (e) =>
          e.extensionId && e.extensionId.trim() !== '' && e.pluginName && e.pluginName.trim() !== '',
      )
      .map(normalizeJsonEntry);

    // Compare as plain objects (EXTENSIONS_CATALOG is readonly; spread to mutable for deep-equal).
    const actual = EXTENSIONS_CATALOG.map((d) => ({
      extensionId: d.extensionId,
      name: d.name,
      description: d.description,
      pluginName: d.pluginName,
      repo: d.repo,
      version: d.version,
      minCoreVersion: d.minCoreVersion,
      enginePlugins: [...d.enginePlugins],
      tools: d.tools.map((t) => ({ name: t.name, description: t.description })),
    }));

    expect(
      actual,
      'cli/src/utils/extensions-catalog.ts drifted from cli/extensions.catalog.json. ' +
        'Update EXTENSIONS_CATALOG to match the JSON source of truth.',
    ).toEqual(expected);
  });
});
