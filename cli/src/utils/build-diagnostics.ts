/** Lines that state why a UBT build failed. Source diagnostics (clang, MSVC, UHT) point at a file. */
const SOURCE_DIAGNOSTIC = [
  /:\d+(?::\d+)?:\s+(?:fatal\s+)?error:/i, // clang: File.cpp:11:38: error: …
  /\(\d+(?:,\d+)?\)\s*:\s+(?:fatal\s+)?error\b/i, // MSVC / UHT: File.cpp(11): error C2065: … / File.h(18): Error: …
];
const BUILD_DIAGNOSTIC = [
  /:\s+(?:fatal\s+)?error\s+(?:LNK|C)\d+:/i, // MSVC linker or a diagnostic without a location
  /^\s*ld: |^Undefined symbols for architecture|linker command failed/,
  /^\s*ERROR:\s|BuildException:|is not a valid platform to build|Unable to find valid SDK/,
];
/** What clang prints under a diagnostic: the source excerpt (`11 |  code`, `   |  ^~~`) and its notes. */
const EXCERPT = /^\s*\d*\s+\||:\d+(?::\d+)?:\s+note:/;
const MAX_DIAGNOSTIC_LINES = 40;
const MAX_DIAGNOSTIC_CHARS = 6000;

export interface UnrealBuildDiagnostics {
  /** Feed one output line (without its line break). */
  line(raw: string): void;
  /** The failure message: the errors first; the raw output tail only when no diagnostic was recognised. */
  message(code: number | null, tail: string): string;
}

/**
 * Collects a failed build's diagnostics from the WHOLE output: in a large project the first error is printed
 * long before the build ends, so the last few KB of output hold only progress lines.
 */
export function createUnrealBuildDiagnostics(): UnrealBuildDiagnostics {
  const lines: string[] = [];
  const seen = new Set<string>();
  let chars = 0, excerpt = 0, errors = 0, sourceErrors = 0, omitted = 0;
  let logFile: string | undefined, result: string | undefined;
  const keep = (line: string): boolean => {
    if (lines.length >= MAX_DIAGNOSTIC_LINES || chars + line.length > MAX_DIAGNOSTIC_CHARS) return false;
    lines.push(line);
    chars += line.length + 1;
    return true;
  };
  return {
    line(raw) {
      const line = raw.trimEnd();
      if (!line.trim()) return;
      const log = /^\s*Log file:\s*(.+)$/.exec(line);
      if (log) { logFile = log[1]!.trim(); return; }
      const res = /^\s*Result:\s*(.+)$/.exec(line);
      if (res) { result = res[1]!.trim(); return; }
      const source = SOURCE_DIAGNOSTIC.some((pattern) => pattern.test(line));
      if (source || BUILD_DIAGNOSTIC.some((pattern) => pattern.test(line))) {
        excerpt = 0;
        if (seen.has(line)) return; // UBT can repeat an error in its summary
        seen.add(line);
        errors++;
        if (source) sourceErrors++;
        if (keep(line)) excerpt = 6;
        else omitted++;
      } else if (excerpt > 0 && EXCERPT.test(line)) {
        excerpt--;
        keep(line);
      } else excerpt = 0;
    },
    message(code, tail) {
      const head = `Unreal C++ build failed (exit code ${code}${result ? `, ${result}` : ''}). The editor was not started.`;
      const log = logFile ? `\nFull build log: ${logFile}` : '';
      if (errors === 0) return `${head}${log}\nLast build output:\n${tail.trim().slice(-4096)}`;
      const more = omitted ? `\n… ${omitted} more error${omitted === 1 ? '' : 's'} in the build log.` : '';
      const next = sourceErrors ? "Fix the errors in the project's source files" : 'Fix the build problem above';
      return `${head}\n${errors} error${errors === 1 ? '' : 's'}:\n${lines.join('\n')}${more}\n${next}, then open the editor again.${log}`;
    },
  };
}
