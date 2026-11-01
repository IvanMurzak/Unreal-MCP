import { describe, it, expect } from 'vitest';
import { createUnrealBuildDiagnostics } from '../src/utils/build-diagnostics.js';
import { runUnrealBuildStep } from '../src/utils/unreal-build.js';

describe('Unreal build diagnostics', () => {
  it('keeps a compiler error printed early in a large build instead of the last output', async () => {
    const script = [
      'console.log("Log file: /Users/dev/Library/Application Support/Epic/UnrealBuildTool/Log.txt")',
      'console.log("/p/Source/Game/SpinningActor.cpp:11:38: error: use of undeclared identifier \'RotationSped\'; did you mean \'RotationSpeed\'?")',
      'console.log("   11 |     AddActorLocalRotation(FRotator(0.f, RotationSped * DeltaSeconds, 0.f));")',
      'console.log("/p/Source/Game/SpinningActor.h:18:8: note: \'RotationSpeed\' declared here")',
      'for (let i = 3; i <= 160; i++) console.log(`[${i}/160] Compile [Apple] Filler${i}.cpp`)',
      'console.log("Result: Failed (OtherCompilationError)")',
      'process.exit(6)',
    ].join(';');
    const failure = await runUnrealBuildStep({
      ubtPath: process.execPath, editorTarget: 'TestEditor', uprojectPath: 'Test.uproject', args: ['-e', script],
    }).then(() => undefined, (error: Error) => error);
    expect(failure?.message.split('\n').slice(0, 5)).toEqual([
      'Unreal C++ build failed (exit code 6, Failed (OtherCompilationError)). The editor was not started.',
      '1 error:',
      "/p/Source/Game/SpinningActor.cpp:11:38: error: use of undeclared identifier 'RotationSped'; did you mean 'RotationSpeed'?",
      '   11 |     AddActorLocalRotation(FRotator(0.f, RotationSped * DeltaSeconds, 0.f));',
      "/p/Source/Game/SpinningActor.h:18:8: note: 'RotationSpeed' declared here",
    ]);
    expect(failure?.message).toContain("Fix the errors in the project's source files, then open the editor again.");
    expect(failure?.message).toContain('Full build log: /Users/dev/Library/Application Support/Epic/UnrealBuildTool/Log.txt');
    expect(failure?.message).not.toContain('Filler');
  });

  it('recognises MSVC, UHT and linker failures, deduplicated and bounded', () => {
    const d = createUnrealBuildDiagnostics();
    for (const line of [
      "D:\\Game\\Source\\Game\\Spin.cpp(11): error C2065: 'RotationSped': undeclared identifier",
      "D:\\Game\\Source\\Game\\Spin.h(18): Error: Unrecognized type 'FFoo' - type must be a UCLASS, USTRUCT, UENUM, or global delegate.",
      'Module.Game.cpp.obj : error LNK2019: unresolved external symbol "void Foo(void)"',
      "D:\\Game\\Source\\Game\\Spin.cpp(11): error C2065: 'RotationSped': undeclared identifier",
      'D:\\Game\\Source\\Game\\Other.cpp(3): warning C4996: deprecated',
    ]) d.line(line);
    const message = d.message(6, 'tail');
    expect(message).toContain('3 errors:');
    expect(message.match(/C2065/g)).toHaveLength(1);
    expect(message).not.toContain('C4996');
    const many = createUnrealBuildDiagnostics();
    for (let i = 0; i < 100; i++) many.line(`/p/F${i}.cpp:1:1: error: broken ${i}`);
    expect(many.message(6, '')).toMatch(/100 errors:[\s\S]*… 60 more errors in the build log\./);
  });

  it('words toolchain failures without blaming the source and falls back to the raw tail', () => {
    const sdk = createUnrealBuildDiagnostics();
    sdk.line('Platform Mac is not a valid platform to build. Check that the SDK is installed properly.');
    expect(sdk.message(6, '')).toContain('Fix the build problem above, then open the editor again.');
    const unknown = createUnrealBuildDiagnostics();
    unknown.line('Something unexpected');
    expect(unknown.message(1, 'Something unexpected')).toBe(
      'Unreal C++ build failed (exit code 1). The editor was not started.\nLast build output:\nSomething unexpected');
  });
});
