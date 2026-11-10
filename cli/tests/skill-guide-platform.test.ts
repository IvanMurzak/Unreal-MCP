import { describe, expect, it } from 'vitest';
import { buildGuideFiles, buildInstallGuide } from '../src/lib/skill-guides.js';

describe('platform-specific install reference', () => {
  for (const platform of ['win32', 'linux', 'darwin'] as const) {
    it(`writes only the commands for ${platform}`, () => {
      const guide = buildInstallGuide(platform);
      const windows = platform === 'win32';
      expect(guide).toContain(windows ? '```powershell' : '```bash');
      expect(guide).not.toContain(windows ? '```bash' : '```powershell');
      expect(guide).toContain(windows ? 'npm.cmd install -g' : 'npm install -g');
      expect(guide).toContain('install-plugin');
      expect(guide).not.toMatch(/@[A-Z_]+@/);
      const file = buildGuideFiles(platform).find(f => f.relativePath === 'unreal-mcp-cli/references/install.md')!;
      expect(file.content).toBe(guide);
    });
  }
});
