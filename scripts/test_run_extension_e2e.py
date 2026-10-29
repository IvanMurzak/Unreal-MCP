from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import extension_release as release
import run_extension_e2e as e2e

COUNT = {'content': [{'type': 'text', 'text': '3'}], 'isError': False}
STRUCTURED_COUNT = {'content': [{'type': 'text', 'text': 'ok'}], 'structuredContent': {'result': {'kind': 'scalar', 'value': 0}}}
ESCAPED_COUNT = {'content': [{'type': 'text', 'text': json.dumps({'kind': 'scalar', 'value': 12})}]}
NO_COUNT = {'content': [{'type': 'text', 'text': 'no answer'}], 'isError': False}


def text_result(text):
    return {'content': [{'type': 'text', 'text': text}], 'isError': False}


def all_checks():
    return {(plugin, path.name): e2e.load_check(path) for plugin in release.components() for path in e2e.checks_for(plugin)}


def matches(check, result):
    try:
        e2e.evaluate(check, 0, json.dumps(result, indent=2))
        return True
    except ValueError:
        return False


class CheckFileTests(unittest.TestCase):
    def test_every_plugin_has_valid_checks_and_no_powershell_left(self):
        for plugin, folder in release.components().items():
            self.assertTrue(e2e.checks_for(plugin), plugin)
            self.assertEqual(list((folder / 'Tests/e2e/tools').glob('*.ps1')), [], plugin)
        self.assertEqual(len(all_checks()), 19)

    def test_every_assert_matches_its_intended_shape_and_rejects_others(self):
        for (plugin, name), check in all_checks().items():
            with self.subTest(check=f'{plugin}/{name}'):
                pattern = check['assert']['pattern']
                if 'count' in check['input'].get('query', ''):
                    # A count answer arrives as the text of a content item, a structured value, or a value in text JSON.
                    for good in (COUNT, STRUCTURED_COUNT, ESCAPED_COUNT):
                        self.assertTrue(matches(check, good), good)
                    self.assertFalse(matches(check, NO_COUNT))
                    self.assertFalse(matches(check, text_result('value: none')))
                elif check['tool'] == 'rql-schema':
                    # The schema lists the expected member/operation by name.
                    self.assertTrue(matches(check, text_result(f'Members: Name, {pattern}(Target)')))
                    self.assertFalse(matches(check, NO_COUNT))
                else:
                    # inputContexts rows: PowerShell -match is case-insensitive, so any "result" text passes.
                    self.assertEqual(pattern, 'result')
                    self.assertTrue(matches(check, {'structuredContent': {'result': {'kind': 'rows', 'rows': []}}}))
                    self.assertFalse(matches(check, NO_COUNT))

    def test_schema_rejects_malformed_checks(self):
        bad = [
            {'tool': 'rql-query'},
            {'description': 'x', 'tool': 'Bad_Tool'},
            {'description': 'x', 'tool': 'rql-query', 'input': '{"query":"a"}'},
            {'description': 'x', 'tool': 'rql-query', 'system': 'no'},
            {'description': 'x', 'tool': 'rql-query', 'expectError': True},
            {'description': 'x', 'tool': 'rql-query', 'assert': {'pattern': '('}},
            {'description': 'x', 'tool': 'rql-query', 'assert': {'notPattern': 'a'}},
            {'description': 'x', 'tool': 'rql-query', 'Assert': {'pattern': 'a'}},
        ]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'bad.e2e.json'
            for check in bad:
                path.write_text(json.dumps(check), encoding='utf-8')
                with self.subTest(check=check), self.assertRaises(ValueError):
                    e2e.load_check(path)

    def test_serialization_is_compact_and_keeps_non_ascii(self):
        self.assertEqual(e2e.serialize({'a': [1, 'é']}), '{"a":[1,"é"]}')


class RunnerTests(unittest.TestCase):
    CHECK = {'description': 'the count', 'tool': 'rql-query', 'input': {'query': 'a | count'},
             'assert': {'pattern': r'"text"\s*:\s*"\d+"'}}

    def run_with(self, checks, outputs):
        """Run plugin 'P' with the given checks against a fake CLI returning (code, text) per call."""
        calls = []

        def fake_run(args, **kwargs):
            calls.append(args)
            code, text = outputs[len(calls) - 1]
            return subprocess.CompletedProcess(args, code, stdout=text)

        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary) / 'extensions/P/Tests/e2e/tools'
            folder.mkdir(parents=True)
            for index, check in enumerate(checks):
                (folder / f'c{index}.e2e.json').write_text(json.dumps(check), encoding='utf-8')
            with patch.object(e2e, 'ROOT', Path(temporary)), patch.object(e2e.subprocess, 'run', fake_run), \
                    redirect_stdout(io.StringIO()):
                failures = e2e.run('/proj', 'http://localhost:1', ['P'])
        return failures, calls

    def test_success_builds_the_cli_call(self):
        failures, calls = self.run_with([self.CHECK, dict(self.CHECK, system=True, input={})],
                                        [(0, json.dumps(COUNT)), (0, json.dumps(COUNT))])
        self.assertEqual(failures, [])
        self.assertEqual(calls[0][2:], ['run-tool', 'rql-query', '--path', '/proj', '--url', 'http://localhost:1',
                                        '--input', '{"query":"a | count"}'])
        self.assertEqual(calls[1][2], 'run-system-tool')
        self.assertEqual(calls[1][-2:], ['--input', '{}'])

    def test_missing_checks_fail(self):
        failures, calls = self.run_with([], [])
        self.assertEqual(calls, [])
        self.assertIn('Missing E2E checks', failures[0])

    def test_failure_rules(self):
        cases = [
            ((1, 'ERROR: boom'), 'CLI failed'),
            ((0, 'ECONNREFUSED 127.0.0.1'), 'Transport failure'),
            ((0, 'not json'), 'Invalid success'),
            ((0, '[1]'), 'Invalid success'),
            ((0, json.dumps(dict(COUNT, isError=True))), 'Invalid success'),
            ((0, json.dumps(NO_COUNT)), 'expected the count'),
        ]
        for output, message in cases:
            with self.subTest(output=output):
                failures, _ = self.run_with([self.CHECK], [output])
                self.assertEqual(len(failures), 1)
                self.assertIn('P/rql-query: ' + message, failures[0])

    def test_expected_error_rules(self):
        check = {'description': 'the not-found error', 'tool': 'rql-query', 'expectError': True,
                 'assert': {'pattern': 'unknown source'}}
        failure = 'ERROR: Failed to run tool "rql-query" (http): Unknown source "x"'
        self.assertEqual(self.run_with([check], [(1, failure)])[0], [])
        self.assertIn('expected the not-found error', self.run_with([check], [(1, 'ERROR: other')])[0][0])
        transport = 'ERROR: Failed to run tool "rql-query" (network): unknown source'
        self.assertIn('Transport failure', self.run_with([check], [(1, transport)])[0][0])
        missing_assert = {'description': 'x', 'tool': 'rql-query', 'expectError': True}
        self.assertIn('must assert', self.run_with([missing_assert], [])[0][0])

    def test_one_failure_does_not_stop_later_checks(self):
        failures, calls = self.run_with([self.CHECK, self.CHECK], [(1, 'ERROR: boom'), (0, json.dumps(COUNT))])
        self.assertEqual(len(calls), 2)
        self.assertEqual(len(failures), 1)


if __name__ == '__main__':
    unittest.main()
