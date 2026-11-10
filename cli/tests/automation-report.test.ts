import { describe, expect, it } from 'vitest';
import { failingTests, parseAutomationReport, toGitHubAnnotations, toJUnitXml } from '../src/utils/automation-report.js';

const REPORT = {
  succeeded: 1,
  failed: 1,
  notRun: 1,
  totalDuration: 2.5,
  tests: [
    { testDisplayName: 'Passes', fullTestPath: 'Project.Cli.Passes', state: 'Success', duration: 0.5, entries: [] },
    {
      testDisplayName: 'Fails',
      fullTestPath: 'Project.Cli.Fails',
      state: 'Fail',
      duration: 1.5,
      entries: [
        { event: { type: 'Error', message: 'Expected 1, got 2, 100%' }, filename: 'C:/P/Source/CliTest.cpp', lineNumber: 42 },
        { event: { type: 'Warning', message: 'slow' }, filename: '', lineNumber: -1 },
      ],
    },
    { testDisplayName: 'Skipped', fullTestPath: 'Project.Cli.Skipped', state: 'NotRun', duration: 0, entries: [] },
  ],
};

describe('parseAutomationReport', () => {
  it('counts passed, failed and not-run tests and keeps error locations', () => {
    const summary = parseAutomationReport(`\uFEFF${JSON.stringify(REPORT)}`);
    expect(summary).toMatchObject({ total: 3, passed: 1, failed: 1, notRun: 1, durationSeconds: 2.5 });
    const failing = failingTests(summary);
    expect(failing.map((t) => t.fullPath)).toEqual(['Project.Cli.Fails', 'Project.Cli.Skipped']);
    expect(failing[0]!.errors).toEqual([{ type: 'Error', message: 'Expected 1, got 2, 100%', file: 'C:/P/Source/CliTest.cpp', line: 42 }]);
    expect(failing[0]!.warnings).toEqual([{ type: 'Warning', message: 'slow' }]);
  });

  it('rejects a file that is not a report (or still being written)', () => {
    expect(() => parseAutomationReport('{"tests": [')).toThrow();
    expect(() => parseAutomationReport('{}')).toThrow(/tests/);
  });
});

describe('report renderers', () => {
  const summary = parseAutomationReport(JSON.stringify(REPORT));

  it('JUnit XML: failures, skipped tests and escaping', () => {
    const xml = toJUnitXml(summary);
    expect(xml).toContain('<testsuite name="UnrealAutomation" tests="3" failures="1" skipped="1" time="2.500">');
    expect(xml).toContain('<testcase classname="Project.Cli" name="Passes" time="0.500"/>');
    expect(xml).toContain('<failure message="C:/P/Source/CliTest.cpp:42: Expected 1, got 2, 100%">');
    expect(xml).toContain('<skipped message="NotRun"/>');
    expect(toJUnitXml(parseAutomationReport(JSON.stringify({ tests: [{ fullTestPath: 'A.<b>&"c"', state: 'Fail', entries: [] }] }))))
      .toContain('name="&lt;b&gt;&amp;&quot;c&quot;"');
  });

  it('GitHub annotations: file, line and title, escaped', () => {
    expect(toGitHubAnnotations(summary)).toEqual([
      '::error file=C%3A/P/Source/CliTest.cpp,line=42,title=Project.Cli.Fails::Expected 1, got 2, 100%25',
      '::error title=Project.Cli.Skipped::Test ended in state NotRun.',
    ]);
  });
});
