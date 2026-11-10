// Structured diagnostics from UnrealBuildTool output (`compile`): MSVC, clang, UHT and linker lines.

export interface CompilerDiagnostic {
  severity: 'error' | 'warning';
  /** Source file, when the line names one. */
  file?: string;
  line?: number;
  column?: number;
  /** Compiler code (`C2065`, `LNK2019`, `-Wunused-variable`), when given. */
  code?: string;
  message: string;
}

// MSVC / UHT: C:\P\Foo.cpp(11,5): error C2065: 'x': undeclared identifier   |   Foo.h(18): Error: ...
const MSVC = /^\s*(.+?)\((\d+)(?:,(\d+))?\)\s*:\s+(fatal error|error|warning)\s*([A-Z]+\d+)?\s*:\s*(.*)$/i;
// clang: /p/Foo.cpp:11:38: error: use of undeclared identifier 'x' [-Wsomething]
const CLANG = /^\s*(.+?):(\d+):(?:(\d+):)?\s+(fatal error|error|warning):\s*(.*?)(?:\s+\[(-W[^\]]+)\])?$/i;
// A diagnostic without a source location: LINK : fatal error LNK1181: ... / Module.obj : error LNK2019: ...
const NO_LOCATION = /^\s*(.+?)\s*:\s+(fatal error|error|warning)\s+((?:LNK|C)\d+)\s*:\s*(.*)$/i;
// UBT's own failures: ERROR: <message>   (and the macOS / Linux linker's `ld: ...`)
const UBT_ERROR = /^\s*(?:ERROR:\s+(.*)|ld: (?!warning)(.*)|(Undefined symbols for architecture .*))$/;

function severityOf(word: string): 'error' | 'warning' {
  return /warning/i.test(word) ? 'warning' : 'error';
}

/** Parse one output line into a diagnostic, or `undefined` for every other line. */
export function parseDiagnosticLine(raw: string): CompilerDiagnostic | undefined {
  const line = raw.trimEnd();
  let m = MSVC.exec(line);
  if (m) {
    return {
      severity: severityOf(m[4]!),
      file: m[1]!.trim(),
      line: Number(m[2]),
      ...(m[3] ? { column: Number(m[3]) } : {}),
      ...(m[5] ? { code: m[5] } : {}),
      message: m[6]!.trim(),
    };
  }
  m = CLANG.exec(line);
  if (m) {
    return {
      severity: severityOf(m[4]!),
      file: m[1]!.trim(),
      line: Number(m[2]),
      ...(m[3] ? { column: Number(m[3]) } : {}),
      ...(m[6] ? { code: m[6] } : {}),
      message: m[5]!.trim(),
    };
  }
  m = NO_LOCATION.exec(line);
  if (m) return { severity: severityOf(m[2]!), code: m[3]!, message: `${m[1]!.trim()}: ${m[4]!.trim()}` };
  m = UBT_ERROR.exec(line);
  if (m) return { severity: 'error', message: (m[1] ?? m[2] ?? m[3])!.trim() };
  return undefined;
}

/** Collects the unique diagnostics of a build (UBT repeats errors in its summary). */
export class DiagnosticCollector {
  readonly diagnostics: CompilerDiagnostic[] = [];
  private readonly seen = new Set<string>();

  line(raw: string): CompilerDiagnostic | undefined {
    const diagnostic = parseDiagnosticLine(raw);
    if (!diagnostic) return undefined;
    const key = `${diagnostic.severity}|${diagnostic.file ?? ''}|${diagnostic.line ?? ''}|${diagnostic.column ?? ''}|${diagnostic.message}`;
    if (this.seen.has(key)) return undefined;
    this.seen.add(key);
    this.diagnostics.push(diagnostic);
    return diagnostic;
  }

  get errors(): CompilerDiagnostic[] {
    return this.diagnostics.filter((d) => d.severity === 'error');
  }

  get warnings(): CompilerDiagnostic[] {
    return this.diagnostics.filter((d) => d.severity === 'warning');
  }
}

/** `file:line:col: severity CODE: message` — the clickable form editors and terminals understand. */
export function formatDiagnostic(d: CompilerDiagnostic): string {
  const location = d.file ? `${d.file}${d.line !== undefined ? `:${d.line}` : ''}${d.column !== undefined ? `:${d.column}` : ''}: ` : '';
  return `${location}${d.severity}${d.code ? ` ${d.code}` : ''}: ${d.message}`;
}
