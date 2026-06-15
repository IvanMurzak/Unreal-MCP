import { describe, it, expect } from 'vitest';
import { autoInstallEngine, engineInstallGuidance } from '../src/lib/auto-install-engine.js';

describe('engineInstallGuidance', () => {
  it('points Windows/macOS at the Epic launcher deep link', () => {
    const win = engineInstallGuidance('5.7', 'win32', 'com.epicgames.launcher://ue/');
    expect(win.join('\n')).toContain('Epic Games Launcher');
    expect(win.join('\n')).toContain('com.epicgames.launcher://ue/');
    const mac = engineInstallGuidance('5.7', 'darwin', 'com.epicgames.launcher://ue/');
    expect(mac.join('\n')).toContain('Epic Games Launcher');
  });
  it('points Linux at the login-gated official download page', () => {
    const lin = engineInstallGuidance('5.7', 'linux', 'com.epicgames.launcher://ue/');
    expect(lin.join('\n')).toContain('login-gated');
    expect(lin.join('\n')).toContain('unrealengine.com');
  });
});

describe('autoInstallEngine — already installed short-circuit (validated)', () => {
  it('returns already-installed with the resolved binary', () => {
    const r = autoInstallEngine({
      version: '5.7',
      os: 'win32',
      resolveInstalledImpl: () => 'C:\\UE\\UnrealEditor.exe',
    });
    expect(r.outcome).toBe('already-installed');
    expect(r.editorPath).toBe('C:\\UE\\UnrealEditor.exe');
  });
});

describe('autoInstallEngine — consent gating (never silent multi-GB)', () => {
  it('refuses without consent in a non-interactive context', () => {
    const r = autoInstallEngine({
      version: '5.7',
      os: 'win32',
      consent: false,
      interactive: false,
      resolveInstalledImpl: () => null,
    });
    expect(r.outcome).toBe('consent-required');
    expect(r.editorPath).toBeNull();
    expect(r.message).toContain('--yes');
  });

  it('with explicit consent, degrades to guidance (no unattended installer exists)', () => {
    const r = autoInstallEngine({
      version: '5.7',
      os: 'win32',
      consent: true,
      resolveInstalledImpl: () => null,
    });
    expect(r.outcome).toBe('guidance-only');
    expect(r.editorPath).toBeNull();
    expect(r.guidance.length).toBeGreaterThan(0);
  });

  it('an interactive terminal counts as consent', () => {
    const r = autoInstallEngine({
      version: '5.7',
      os: 'linux',
      interactive: true,
      resolveInstalledImpl: () => null,
    });
    expect(r.outcome).toBe('guidance-only');
  });

  it('always surfaces the launcher deep link + per-OS guidance', () => {
    const r = autoInstallEngine({ version: '5.7', os: 'darwin', resolveInstalledImpl: () => null });
    expect(r.launcherUrl).toBe('com.epicgames.launcher://ue/');
    expect(r.guidance.length).toBeGreaterThan(0);
  });

  it('never claims an install succeeded when no binary resolves', () => {
    const r = autoInstallEngine({
      version: '5.7',
      os: 'win32',
      consent: true,
      resolveInstalledImpl: () => null, // nothing got installed
    });
    expect(r.editorPath).toBeNull();
    expect(r.outcome).not.toBe('already-installed');
  });
});
