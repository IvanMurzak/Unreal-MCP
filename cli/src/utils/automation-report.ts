// Unreal Automation reports (`-ReportExportPath=<dir>` writes `<dir>/index.json`): the verdict of `test`, its JUnit
// XML and its GitHub Actions annotations. Pure.

/** One log entry of a test (`entries[]`). */
export interface AutomationEntry {
  type: string;
  message: string;
  file?: string;
  line?: number;
}

export interface AutomationTest {
  /** `Project.Foo.Bar` */
  fullPath: string;
  displayName: string;
  /** `Success`, `SuccessWithWarnings`, `Fail`, `NotRun`, `InProcess`, `Skipped`, ... */
  state: string;
  passed: boolean;
  durationSeconds: number;
  errors: AutomationEntry[];
  warnings: AutomationEntry[];
}

export interface AutomationSummary {
  total: number;
  passed: number;
  failed: number;
  /** Not run, skipped or still in process when the report was written. */
  notRun: number;
  durationSeconds: number;
  tests: AutomationTest[];
}

const PASSED_STATES = new Set(['Success', 'SuccessWithWarnings']);
const NOT_RUN_STATES = new Set(['NotRun', 'Skipped', 'InProcess', 'NotEnoughParticipants']);

interface RawEntry {
  event?: { type?: unknown; message?: unknown };
  filename?: unknown;
  lineNumber?: unknown;
}

interface RawTest {
  testDisplayName?: unknown;
  fullTestPath?: unknown;
  state?: unknown;
  duration?: unknown;
  entries?: unknown;
}

function entryOf(raw: RawEntry): AutomationEntry {
  const file = typeof raw.filename === 'string' && raw.filename.trim() ? raw.filename : undefined;
  const line = typeof raw.lineNumber === 'number' && raw.lineNumber > 0 ? raw.lineNumber : undefined;
  return {
    type: typeof raw.event?.type === 'string' ? raw.event.type : 'Info',
    message: typeof raw.event?.message === 'string' ? raw.event.message : '',
    ...(file ? { file } : {}),
    ...(line ? { line } : {}),
  };
}

/** Parse the text of `index.json` (UE writes it with a BOM). Throws on malformed JSON or a report without `tests`. */
export function parseAutomationReport(text: string): AutomationSummary {
  const data = JSON.parse(text.replace(/^﻿/, '')) as { tests?: unknown; totalDuration?: unknown };
  if (!data || typeof data !== 'object' || !Array.isArray(data.tests)) throw new Error('Not an Automation report: no "tests" array.');
  const tests = (data.tests as RawTest[]).map((raw): AutomationTest => {
    const state = typeof raw.state === 'string' ? raw.state : 'Unknown';
    const entries = (Array.isArray(raw.entries) ? (raw.entries as RawEntry[]) : []).map(entryOf);
    const fullPath = typeof raw.fullTestPath === 'string' ? raw.fullTestPath : String(raw.testDisplayName ?? '');
    return {
      fullPath,
      displayName: typeof raw.testDisplayName === 'string' ? raw.testDisplayName : fullPath,
      state,
      passed: PASSED_STATES.has(state),
      durationSeconds: typeof raw.duration === 'number' ? raw.duration : 0,
      errors: entries.filter((e) => e.type === 'Error'),
      warnings: entries.filter((e) => e.type === 'Warning'),
    };
  });
  const passed = tests.filter((t) => t.passed).length;
  // The report's own counters win when they say more (a test missing from `tests[]` still failed).
  const counter = (key: string): number => {
    const value = (data as Record<string, unknown>)[key];
    return typeof value === 'number' && value > 0 ? value : 0;
  };
  const notRun = Math.max(tests.filter((t) => !t.passed && NOT_RUN_STATES.has(t.state)).length, counter('notRun') + counter('inProcess'));
  return {
    total: tests.length,
    passed,
    failed: Math.max(tests.length - passed - tests.filter((t) => !t.passed && NOT_RUN_STATES.has(t.state)).length, counter('failed')),
    notRun,
    durationSeconds: typeof data.totalDuration === 'number' ? data.totalDuration : tests.reduce((s, t) => s + t.durationSeconds, 0),
    tests,
  };
}

/** The tests that did not pass (failed, or never ran to the end). */
export function failingTests(summary: AutomationSummary): AutomationTest[] {
  return summary.tests.filter((t) => !t.passed);
}

function xmlEscape(text: string): string {
  // Drop the control characters XML 1.0 forbids, then escape the markup ones.
  // eslint-disable-next-line no-control-regex
  return text.replace(/[\x00-\x08\x0B\x0C\x0E-\x1F]/g, '').replace(/[<>&"']/g, (c) =>
    ({ '<': '&lt;', '>': '&gt;', '&': '&amp;', '"': '&quot;', "'": '&apos;' })[c]!);
}

function failureText(test: AutomationTest): string {
  const lines = test.errors.map((e) => `${e.file ? `${e.file}${e.line ? `:${e.line}` : ''}: ` : ''}${e.message}`);
  return lines.length > 0 ? lines.join('\n') : `Test ended in state ${test.state}.`;
}

/** JUnit XML (one `<testsuite>`; the class name is the path without the last segment). */
export function toJUnitXml(summary: AutomationSummary, suiteName = 'UnrealAutomation'): string {
  const out: string[] = ['<?xml version="1.0" encoding="UTF-8"?>'];
  const time = summary.durationSeconds.toFixed(3);
  out.push(`<testsuites name="${xmlEscape(suiteName)}" tests="${summary.total}" failures="${summary.failed}" skipped="${summary.notRun}" time="${time}">`);
  out.push(`  <testsuite name="${xmlEscape(suiteName)}" tests="${summary.total}" failures="${summary.failed}" skipped="${summary.notRun}" time="${time}">`);
  for (const test of summary.tests) {
    const dot = test.fullPath.lastIndexOf('.');
    const className = dot > 0 ? test.fullPath.slice(0, dot) : suiteName;
    const name = dot > 0 ? test.fullPath.slice(dot + 1) : test.fullPath;
    const open = `    <testcase classname="${xmlEscape(className)}" name="${xmlEscape(name)}" time="${test.durationSeconds.toFixed(3)}"`;
    if (test.passed) {
      out.push(`${open}/>`);
    } else if (NOT_RUN_STATES.has(test.state)) {
      out.push(`${open}>`, `      <skipped message="${xmlEscape(test.state)}"/>`, '    </testcase>');
    } else {
      const text = failureText(test);
      out.push(`${open}>`, `      <failure message="${xmlEscape(text.split('\n')[0]!)}">${xmlEscape(text)}</failure>`, '    </testcase>');
    }
  }
  out.push('  </testsuite>', '</testsuites>', '');
  return out.join('\n');
}

/** Escape a GitHub workflow-command message (`%`, CR, LF). */
function ghData(text: string): string {
  return text.replace(/%/g, '%25').replace(/\r/g, '%0D').replace(/\n/g, '%0A');
}

/** Escape a GitHub workflow-command property value (also `:` and `,`). */
function ghProperty(text: string): string {
  return ghData(text).replace(/:/g, '%3A').replace(/,/g, '%2C');
}

/** A `::error title=..::message` annotation without a source location. */
export function githubError(title: string, message: string): string {
  return `::error title=${ghProperty(title)}::${ghData(message)}`;
}

/** One `::error file=..,line=..,title=..::message` annotation per error of every failing test. */
export function toGitHubAnnotations(summary: AutomationSummary): string[] {
  const lines: string[] = [];
  for (const test of failingTests(summary)) {
    const title = `title=${ghProperty(test.fullPath)}`;
    const entries = test.errors.length > 0 ? test.errors : [{ type: 'Error', message: `Test ended in state ${test.state}.` } as AutomationEntry];
    for (const entry of entries) {
      const props = [
        ...(entry.file ? [`file=${ghProperty(entry.file)}`] : []),
        ...(entry.file && entry.line ? [`line=${entry.line}`] : []),
        title,
      ];
      lines.push(`::error ${props.join(',')}::${ghData(entry.message)}`);
    }
  }
  return lines;
}
