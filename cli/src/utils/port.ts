import { createHash } from 'crypto';

const MIN_PORT = 20000;
const MAX_PORT = 29999;
const PORT_RANGE = MAX_PORT - MIN_PORT + 1;

/**
 * Generate a deterministic localhost port from a project directory.
 *
 * Mirrors the proven Unity/Godot scheme (`GeneratePortFromDirectory`):
 * SHA-256 of the lowercased directory path, first 4 bytes read as a
 * little-endian uint32, modulo the 20000–29999 range. The UnrealMCP
 * plugin (when it lands) will use the same hashing so the CLI can
 * reach a project's local MCP server without reading any config.
 *
 * Pure / no I/O.
 */
export function generatePortFromDirectory(dir: string): number {
  const hash = createHash('sha256').update(dir.toLowerCase()).digest();
  const int32 = hash.readInt32LE(0);
  const uint32 = int32 >>> 0;
  return MIN_PORT + (uint32 % PORT_RANGE);
}
