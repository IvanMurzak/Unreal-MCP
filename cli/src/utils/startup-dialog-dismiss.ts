import { execFile, execFileSync } from 'child_process';
import { platform as nodePlatform } from 'os';

export type DismissPlatform = 'win32' | 'darwin' | 'linux';
export type UnrealStartupDialogKey = 'missing-modules' | 'plugin-incompatible';

export type DismissOutcome =
  | { kind: 'dismissed'; button: string; dialog: UnrealStartupDialogKey }
  | { kind: 'not-found' }
  | { kind: 'error'; message: string };

export const UNSUPPORTED_PLATFORM_PREFIX =
  'Unsupported platform for Unreal startup-dialog auto-dismiss';
export const LINUX_XDOTOOL_MISSING_PREFIX =
  'xdotool not found on PATH';
export const LINUX_WAYLAND_UNSUPPORTED_PREFIX =
  'Linux Wayland sessions are not supported for Unreal startup-dialog auto-dismiss';

const DISMISS_BUTTON_LABEL = 'Yes';

export const WINDOWS_STARTUP_DIALOG_PS_SCRIPT = `
$ErrorActionPreference = 'Stop'
try {
  if (-not ([System.Management.Automation.PSTypeName]'UnrealMcp.StartupDialog.Dismisser').Type) {
    Add-Type -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
namespace UnrealMcp.StartupDialog {
  public static class Dismisser {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr hWndParent, EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr hWnd, StringBuilder lpString, int nMaxCount);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr SendMessageW(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    const uint BM_CLICK = 0x00F5;
    static readonly string[] YesLabels = new[] { "Yes", "&Yes" };

    public static string TryDismiss(int[] unrealPids) {
      var unrealPidSet = new HashSet<uint>();
      for (int i = 0; i < unrealPids.Length; i++) unrealPidSet.Add((uint)unrealPids[i]);
      if (unrealPidSet.Count == 0) return "not-found";
      string result = "not-found";
      EnumWindows((hWnd, lParam) => {
        if (!IsWindowVisible(hWnd)) return true;
        uint procId; GetWindowThreadProcessId(hWnd, out procId);
        if (!unrealPidSet.Contains(procId)) return true;
        var title = ReadText(hWnd);
        if (string.IsNullOrWhiteSpace(title)) return true;
        var body = ReadChildText(hWnd);
        string clicked;
        if (MatchesMissingModules(title, body) && TryClickButton(hWnd, YesLabels, out clicked)) {
          result = "dismissed:missing-modules:" + clicked;
          return false;
        }
        if (MatchesPluginIncompatible(title, body) && TryClickButton(hWnd, YesLabels, out clicked)) {
          result = "dismissed:plugin-incompatible:" + clicked;
          return false;
        }
        return true;
      }, IntPtr.Zero);
      return result;
    }

    static bool MatchesMissingModules(string title, string body) {
      return title.StartsWith("Missing ", StringComparison.OrdinalIgnoreCase)
        && title.IndexOf(" Modules", StringComparison.OrdinalIgnoreCase) >= 0
        && body.IndexOf("would you like to rebuild them now", StringComparison.OrdinalIgnoreCase) >= 0;
    }

    static bool MatchesPluginIncompatible(string title, string body) {
      return title.IndexOf("Incompatible", StringComparison.OrdinalIgnoreCase) >= 0
        && body.IndexOf("load it anyway", StringComparison.OrdinalIgnoreCase) >= 0
        && (title.IndexOf("UnrealMCP", StringComparison.OrdinalIgnoreCase) >= 0
          || body.IndexOf("UnrealMCP", StringComparison.OrdinalIgnoreCase) >= 0);
    }

    static bool TryClickButton(IntPtr parent, string[] labels, out string clickedLabel) {
      clickedLabel = "";
      IntPtr matchedButton = IntPtr.Zero;
      var sb = new StringBuilder(512);
      EnumChildWindows(parent, (hWnd, lParam) => {
        sb.Length = 0;
        GetClassNameW(hWnd, sb, sb.Capacity);
        if (sb.ToString() != "Button") return true;
        sb.Length = 0;
        GetWindowTextW(hWnd, sb, sb.Capacity);
        var text = sb.ToString();
        for (int i = 0; i < labels.Length; i++) {
          if (string.Equals(text, labels[i], StringComparison.OrdinalIgnoreCase)) {
            matchedButton = hWnd;
            clickedLabel = labels[i];
            return false;
          }
        }
        return true;
      }, IntPtr.Zero);
      if (matchedButton == IntPtr.Zero) return false;
      SendMessageW(matchedButton, BM_CLICK, IntPtr.Zero, IntPtr.Zero);
      return true;
    }

    static string ReadText(IntPtr hWnd) {
      var sb = new StringBuilder(512);
      GetWindowTextW(hWnd, sb, sb.Capacity);
      return sb.ToString();
    }

    static string ReadChildText(IntPtr parent) {
      var text = new StringBuilder(1024);
      var sb = new StringBuilder(512);
      EnumChildWindows(parent, (hWnd, lParam) => {
        sb.Length = 0;
        GetWindowTextW(hWnd, sb, sb.Capacity);
        var chunk = sb.ToString();
        if (!string.IsNullOrWhiteSpace(chunk)) {
          if (text.Length > 0) text.Append('\\n');
          text.Append(chunk);
        }
        return true;
      }, IntPtr.Zero);
      return text.ToString();
    }
  }
}
"@
  }
  $unrealPids = @(
    Get-Process -ErrorAction SilentlyContinue |
      Where-Object { $_.ProcessName -like 'UnrealEditor*' } |
      ForEach-Object { [int]$_.Id }
  )
  Write-Output ([UnrealMcp.StartupDialog.Dismisser]::TryDismiss([int[]]$unrealPids))
} catch {
  Write-Output ('error:' + $_.Exception.Message)
}
`;

export const MACOS_STARTUP_DIALOG_APPLESCRIPT = `
on bodyTextFor(w)
  set chunksText to ""
  try
    set chunks to value of every static text of w
    if class of chunks is list then set chunksText to chunks as string
  end try
  return chunksText
end bodyTextFor

on dialogKeyFor(titleText, bodyText)
  if titleText starts with "Missing " and titleText contains " Modules" and bodyText contains "Would you like to rebuild them now" then
    return "missing-modules"
  end if
  if titleText contains "Incompatible" and bodyText contains "load it anyway" and (titleText contains "UnrealMCP" or bodyText contains "UnrealMCP") then
    return "plugin-incompatible"
  end if
  return ""
end dialogKeyFor

on run
  try
    tell application "System Events"
      set unrealProcesses to every process whose name contains "UnrealEditor"
      if (count of unrealProcesses) is 0 then return "not-found"
      repeat with p in unrealProcesses
        tell p
          repeat with w in windows
            set titleText to ""
            try
              set titleText to name of w
            end try
            set bodyText to my bodyTextFor(w)
            set dialogKey to my dialogKeyFor(titleText, bodyText)
            if dialogKey is not "" then
              if exists (button "${DISMISS_BUTTON_LABEL}" of w) then
                click button "${DISMISS_BUTTON_LABEL}" of w
                return "dismissed:" & dialogKey & ":${DISMISS_BUTTON_LABEL}"
              end if
            end if
          end repeat
        end tell
      end repeat
    end tell
    return "not-found"
  on error errMsg
    return "error:" & errMsg
  end try
end run
`;

let xdotoolPresence: boolean | undefined;

export async function tryDismissUnrealStartupDialog(
  platform: DismissPlatform = nodePlatform() as DismissPlatform,
): Promise<DismissOutcome> {
  switch (platform) {
    case 'win32':
      return tryDismissWindows();
    case 'darwin':
      return tryDismissMacOS();
    case 'linux':
      return tryDismissLinuxX11();
    default:
      return { kind: 'error', message: `${UNSUPPORTED_PLATFORM_PREFIX}: ${platform as string}` };
  }
}

export function _parseDismissOutputForTests(stdout: string): DismissOutcome {
  return parseDismissOutput(stdout);
}

export function _resetXdotoolPresenceForTests(): void {
  xdotoolPresence = undefined;
}

function tryDismissWindows(): Promise<DismissOutcome> {
  return runCommandForDismiss('powershell', ['-NoProfile', '-NonInteractive', '-Command', WINDOWS_STARTUP_DIALOG_PS_SCRIPT], {
    timeout: 5000,
    windowsHide: true,
  });
}

function tryDismissMacOS(): Promise<DismissOutcome> {
  return runCommandForDismiss('osascript', ['-e', MACOS_STARTUP_DIALOG_APPLESCRIPT], {
    timeout: 5000,
  });
}

function tryDismissLinuxX11(): Promise<DismissOutcome> {
  if (process.env['WAYLAND_DISPLAY'] || process.env['XDG_SESSION_TYPE'] === 'wayland') {
    return Promise.resolve({
      kind: 'error',
      message: `${LINUX_WAYLAND_UNSUPPORTED_PREFIX}. Use an X11 session to enable automatic Unreal startup-dialog dismissal.`,
    });
  }
  if (!isXdotoolAvailable()) {
    return Promise.resolve({
      kind: 'error',
      message: `${LINUX_XDOTOOL_MISSING_PREFIX}. Install it (e.g. \`sudo apt-get install xdotool\`) to enable Unreal startup-dialog auto-dismiss on Linux/X11.`,
    });
  }
  const unrealPids = new Set(getUnrealEditorPidsLinux());
  if (unrealPids.size === 0) return Promise.resolve({ kind: 'not-found' });

  return new Promise<DismissOutcome>((resolve) => {
    const patterns: Array<{ dialog: UnrealStartupDialogKey; titlePattern: string }> = [
      { dialog: 'missing-modules', titlePattern: '^Missing .* Modules$' },
      { dialog: 'plugin-incompatible', titlePattern: '.*UnrealMCP.*Incompatible.*|.*Incompatible.*UnrealMCP.*' },
    ];
    let idx = 0;
    const tryNext = (): void => {
      if (idx >= patterns.length) {
        resolve({ kind: 'not-found' });
        return;
      }
      const pattern = patterns[idx++];
      execFile(
        'xdotool',
        ['search', '--name', pattern.titlePattern],
        { timeout: 2000 },
        (err, stdout) => {
          if (err || !stdout.trim()) {
            tryNext();
            return;
          }
          const candidateIds = stdout.trim().split(/\s+/).filter(Boolean);
          findOwnedWindow(candidateIds, unrealPids, (windowId) => {
            if (!windowId) {
              tryNext();
              return;
            }
            execFile(
              'xdotool',
              ['windowactivate', '--sync', windowId, 'key', '--clearmodifiers', 'Return'],
              { timeout: 2000 },
              (activateErr) => {
                if (activateErr) {
                  resolve({
                    kind: 'error',
                    message: `xdotool failed to dismiss window ${windowId}: ${activateErr.message}`,
                  });
                  return;
                }
                resolve({ kind: 'dismissed', dialog: pattern.dialog, button: DISMISS_BUTTON_LABEL });
              },
            );
          });
        },
      );
    };
    tryNext();
  });
}

function isXdotoolAvailable(): boolean {
  if (xdotoolPresence !== undefined) return xdotoolPresence;
  try {
    execFileSync('xdotool', ['--version'], {
      stdio: 'ignore',
      timeout: 2000,
    });
    xdotoolPresence = true;
  } catch {
    xdotoolPresence = false;
  }
  return xdotoolPresence;
}

function getUnrealEditorPidsLinux(): readonly number[] {
  try {
    const stdout = execFileSync('pgrep', ['-f', 'UnrealEditor'], {
      stdio: ['ignore', 'pipe', 'ignore'],
      timeout: 1000,
      encoding: 'utf8',
    });
    return stdout
      .split(/\r?\n/)
      .map((s) => parseInt(s.trim(), 10))
      .filter((n) => Number.isFinite(n) && n > 0);
  } catch {
    return [];
  }
}

function findOwnedWindow(
  candidateIds: string[],
  allowedPids: ReadonlySet<number>,
  done: (windowId: string | undefined) => void,
): void {
  let idx = 0;
  const next = (): void => {
    if (idx >= candidateIds.length) {
      done(undefined);
      return;
    }
    const windowId = candidateIds[idx++];
    execFile('xdotool', ['getwindowpid', windowId], { timeout: 1000 }, (err, stdout) => {
      if (err) {
        next();
        return;
      }
      const pid = parseInt(stdout.trim(), 10);
      if (Number.isFinite(pid) && allowedPids.has(pid)) {
        done(windowId);
        return;
      }
      next();
    });
  };
  next();
}

function runCommandForDismiss(
  file: string,
  args: string[],
  opts: { timeout: number; windowsHide?: boolean },
): Promise<DismissOutcome> {
  return new Promise<DismissOutcome>((resolve) => {
    execFile(file, args, opts, (err, stdout) => {
      if (err) {
        resolve({ kind: 'error', message: err.message });
        return;
      }
      resolve(parseDismissOutput(stdout));
    });
  });
}

function parseDismissOutput(stdout: string): DismissOutcome {
  const last = stdout
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter((line) => line.length > 0)
    .at(-1);
  if (!last || last === 'not-found') return { kind: 'not-found' };
  if (last.startsWith('error:')) {
    return { kind: 'error', message: last.substring('error:'.length) || 'unknown error' };
  }
  if (last.startsWith('dismissed:')) {
    const parts = last.split(':');
    const dialog = (parts[1] ?? '') as UnrealStartupDialogKey;
    const button = parts.slice(2).join(':');
    if (dialog === 'missing-modules' || dialog === 'plugin-incompatible') {
      return { kind: 'dismissed', dialog, button: button || DISMISS_BUTTON_LABEL };
    }
  }
  return { kind: 'error', message: `Unexpected dismiss output: ${last}` };
}
