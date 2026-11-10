import { describe, expect, it } from 'vitest';
import { DiagnosticCollector, formatDiagnostic, parseDiagnosticLine } from '../src/utils/compiler-diagnostics.js';

describe('parseDiagnosticLine', () => {
  it('reads MSVC errors and warnings with line and column', () => {
    expect(parseDiagnosticLine("C:\\P\\Source\\Game\\Foo.cpp(11,5): error C2065: 'x': undeclared identifier")).toEqual({
      severity: 'error', file: 'C:\\P\\Source\\Game\\Foo.cpp', line: 11, column: 5, code: 'C2065', message: "'x': undeclared identifier",
    });
    expect(parseDiagnosticLine("C:\\P\\Seq.cpp(211,69): warning C4996: 'Find': Use the version that takes a SharedPlaybackState")).toMatchObject({
      severity: 'warning', line: 211, column: 69, code: 'C4996',
    });
  });

  it('reads UHT errors without a code', () => {
    expect(parseDiagnosticLine('C:\\P\\Foo.h(18): Error: Missing GENERATED_BODY')).toEqual({
      severity: 'error', file: 'C:\\P\\Foo.h', line: 18, message: 'Missing GENERATED_BODY',
    });
  });

  it('reads clang diagnostics', () => {
    expect(parseDiagnosticLine('/p/Source/Foo.cpp:11:38: error: use of undeclared identifier \'x\'')).toEqual({
      severity: 'error', file: '/p/Source/Foo.cpp', line: 11, column: 38, message: "use of undeclared identifier 'x'",
    });
    expect(parseDiagnosticLine('/p/Foo.cpp:3:7: warning: unused variable \'y\' [-Wunused-variable]')).toMatchObject({
      severity: 'warning', code: '-Wunused-variable', message: "unused variable 'y'",
    });
  });

  it('reads linker and UBT errors without a location', () => {
    expect(parseDiagnosticLine('Module.Game.cpp.obj : error LNK2019: unresolved external symbol foo')).toEqual({
      severity: 'error', code: 'LNK2019', message: 'Module.Game.cpp.obj: unresolved external symbol foo',
    });
    expect(parseDiagnosticLine('ERROR: Unable to build while Live Coding is active.')).toEqual({
      severity: 'error', message: 'Unable to build while Live Coding is active.',
    });
  });

  it('ignores progress lines', () => {
    expect(parseDiagnosticLine('[3/111] Compile [x64] PerModuleInline.gen.cpp')).toBeUndefined();
    expect(parseDiagnosticLine('Result: Succeeded')).toBeUndefined();
    expect(parseDiagnosticLine('  Running Internal UnrealHeaderTool C:\\P\\X.uproject -WarningsAsErrors')).toBeUndefined();
  });
});

describe('DiagnosticCollector', () => {
  it('drops the repeats UBT prints in its summary and splits errors from warnings', () => {
    const c = new DiagnosticCollector();
    const line = "C:\\P\\Foo.cpp(11,5): error C2065: 'x': undeclared identifier";
    expect(c.line(line)).toBeDefined();
    expect(c.line(line)).toBeUndefined();
    c.line('C:\\P\\Foo.cpp(2,1): warning C4100: unreferenced parameter');
    expect(c.errors).toHaveLength(1);
    expect(c.warnings).toHaveLength(1);
    expect(formatDiagnostic(c.errors[0]!)).toBe("C:\\P\\Foo.cpp:11:5: error C2065: 'x': undeclared identifier");
  });
});
