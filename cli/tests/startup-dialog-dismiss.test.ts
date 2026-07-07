import { describe, it, expect } from 'vitest';
import {
  _parseDismissOutputForTests,
  _resetXdotoolPresenceForTests,
  LINUX_WAYLAND_UNSUPPORTED_PREFIX,
  tryDismissUnrealStartupDialog,
  UNSUPPORTED_PLATFORM_PREFIX,
} from '../src/utils/startup-dialog-dismiss.js';

describe('startup-dialog-dismiss', () => {
  it('parses a dismissed missing-modules result', () => {
    expect(_parseDismissOutputForTests('dismissed:missing-modules:Yes')).toEqual({
      kind: 'dismissed',
      dialog: 'missing-modules',
      button: 'Yes',
    });
  });

  it('parses a dismissed plugin-incompatible result', () => {
    expect(_parseDismissOutputForTests('dismissed:plugin-incompatible:Yes')).toEqual({
      kind: 'dismissed',
      dialog: 'plugin-incompatible',
      button: 'Yes',
    });
  });

  it('parses an error result', () => {
    expect(_parseDismissOutputForTests('error:boom')).toEqual({
      kind: 'error',
      message: 'boom',
    });
  });

  it('returns an unsupported-platform error for unknown platforms', async () => {
    const r = await tryDismissUnrealStartupDialog('freebsd' as never);
    expect(r.kind).toBe('error');
    if (r.kind === 'error') {
      expect(r.message).toContain(UNSUPPORTED_PLATFORM_PREFIX);
    }
  });

  it('returns a Wayland limitation warning on Linux Wayland sessions', async () => {
    _resetXdotoolPresenceForTests();
    const originalWayland = process.env['WAYLAND_DISPLAY'];
    const originalSession = process.env['XDG_SESSION_TYPE'];
    process.env['WAYLAND_DISPLAY'] = 'wayland-0';
    process.env['XDG_SESSION_TYPE'] = 'wayland';
    try {
      const r = await tryDismissUnrealStartupDialog('linux');
      expect(r.kind).toBe('error');
      if (r.kind === 'error') {
        expect(r.message).toContain(LINUX_WAYLAND_UNSUPPORTED_PREFIX);
      }
    } finally {
      if (originalWayland === undefined) delete process.env['WAYLAND_DISPLAY'];
      else process.env['WAYLAND_DISPLAY'] = originalWayland;
      if (originalSession === undefined) delete process.env['XDG_SESSION_TYPE'];
      else process.env['XDG_SESSION_TYPE'] = originalSession;
    }
  });
});
