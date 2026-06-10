import { describe, it, expect } from 'vitest';
import { generatePortFromDirectory } from '../src/utils/port.js';

describe('generatePortFromDirectory', () => {
  it('is deterministic and in the 20000-29999 range', () => {
    const a = generatePortFromDirectory('C:\\Projects\\MyGame');
    const b = generatePortFromDirectory('C:\\Projects\\MyGame');
    expect(a).toBe(b);
    expect(a).toBeGreaterThanOrEqual(20000);
    expect(a).toBeLessThanOrEqual(29999);
  });

  it('is case-insensitive (lowercases the path)', () => {
    expect(generatePortFromDirectory('/Foo/Bar')).toBe(generatePortFromDirectory('/foo/bar'));
  });

  it('differs for different directories', () => {
    expect(generatePortFromDirectory('/a')).not.toBe(generatePortFromDirectory('/b'));
  });
});
