import copy
import json
import os
from pathlib import Path
import tempfile
import unittest
import zipfile

import extension_release as release
import extension_suite as suite


class MonorepoTests(unittest.TestCase):
    def test_default_output_is_absolute(self):
        self.assertTrue(suite.default_output().is_absolute())

    @unittest.skipUnless(os.name == 'nt', 'Windows exclusive-file lock')
    def test_local_ue_lock_excludes_a_second_holder_and_releases(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'ue.lock'
            with suite.machine_lock(path=path):
                with self.assertRaises(OSError):
                    with suite.machine_lock(path=path, timeout_seconds=0):
                        self.fail('A second holder acquired the lock')
            with suite.machine_lock(path=path, timeout_seconds=0):
                pass

    def test_package_is_deterministic_source_only_and_contains_one_plugin(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            package = release.package('UnrealAINiagara', output)
            first = package.read_bytes()
            self.assertEqual(first, release.package('UnrealAINiagara', output).read_bytes())
            with zipfile.ZipFile(package) as archive:
                names = archive.namelist()
                self.assertTrue(any(n.endswith('UnrealAINiagara.uplugin') for n in names))
                self.assertTrue(any(n.endswith('UnrealAINiagaraSpec.cpp') for n in names))
                self.assertTrue(all(n.startswith('UnrealAINiagara/') for n in names))
                self.assertFalse(any('/Binaries/' in n or '/Intermediate/' in n for n in names))

    def test_independent_publication_preserves_other_packages_and_history(self):
        index = release.read(release.ROOT / 'extensions-index.json')
        before = copy.deepcopy(index)
        metadata = release.validate('UnrealAINiagara')
        metadata['version'] = '0.2.0'
        result = release.update_index(index, metadata, 'niagara-v0.2.0', 'a' * 64)
        self.assertEqual(index, before)
        old = {e['pluginName']: e for e in before['extensions']}
        new = {e['pluginName']: e for e in result['extensions']}
        for plugin in old:
            if plugin != 'UnrealAINiagara':
                self.assertEqual(old[plugin], new[plugin])
        self.assertEqual(new['UnrealAINiagara']['version'], '0.2.0')
        self.assertIn('0.1.0', new['UnrealAINiagara']['releases'])
        self.assertEqual(new['UnrealAINiagara']['releases']['0.2.0']['tag'], 'niagara-v0.2.0')

    def test_published_versions_cannot_be_overwritten(self):
        index = release.read(release.ROOT / 'extensions-index.json')
        meta = release.validate('UnrealAINiagara')
        index = release.update_index(index, meta, 'niagara-v0.1.0', 'a' * 64)
        self.assertEqual(index, release.update_index(index, meta, 'niagara-v0.1.0', 'a' * 64))
        with self.assertRaisesRegex(ValueError, 'immutable'):
            release.update_index(index, meta, 'niagara-v0.1.0', 'b' * 64)

    def test_isolated_profiles_have_no_sibling_files_and_preserve_spec_sources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            engine = root / 'engine'
            suite.write_json(engine / 'Engine/Build/Build.version', {'MajorVersion': 5, 'MinorVersion': 8})
            project = suite.prepare(engine, 'isolated/Niagara', ['UnrealAINiagara'], root / 'out', strict=True)
            plugins = project.parent / 'Plugins'
            self.assertEqual({p.name for p in plugins.iterdir()}, {'UnrealMCP', 'UnrealAINiagara'})
            self.assertTrue(list((plugins / 'UnrealAINiagara').rglob('*Spec.cpp')))
            rules = next((plugins / 'UnrealAINiagara').rglob('*.Build.cs')).read_text()
            self.assertIn('bUseUnity = false', rules)
            self.assertIn('PCHUsageMode.NoPCHs', rules)
            self.assertNotIn('UnrealMcpEditorTests', (plugins / 'UnrealMCP/UnrealMCP.uplugin').read_text())
            # Plant an unexpected plugin: a reused dirty profile must fail closed.
            (plugins / 'UnrealAIPCG').mkdir()
            with self.assertRaisesRegex(ValueError, 'Unexpected plugin'):
                suite.prepare(engine, 'isolated/Niagara', ['UnrealAINiagara'], root / 'out', strict=True)

    def test_report_gate_catches_missing_extension_and_missing_individual_test(self):
        with tempfile.TemporaryDirectory() as temporary:
            report = Path(temporary) / 'index.json'
            tests = [{'fullTestPath': f'UnrealAINiagara.case{i}', 'state': 'Success'} for i in range(4)]
            suite.write_json(report, {'failed': 0, 'tests': tests})
            self.assertEqual(suite.check_report(report, ['UnrealAINiagara'], core_tests=False)['passed'], 4)
            suite.write_json(report, {'failed': 0, 'tests': tests[:-1]})
            with self.assertRaisesRegex(ValueError, 'Missing Automation coverage'):
                suite.check_report(report, ['UnrealAINiagara'], core_tests=False)
            suite.write_json(report, {'failed': 0, 'tests': tests})
            with self.assertRaisesRegex(ValueError, 'Missing Automation coverage'):
                suite.check_report(report, ['UnrealAINiagara', 'UnrealAIPCG'], core_tests=False)

    def test_missing_report_and_nonpassing_tests_are_failures(self):
        with tempfile.TemporaryDirectory() as temporary:
            report = Path(temporary) / 'index.json'
            with self.assertRaises(FileNotFoundError):
                suite.check_report(report, [])
            suite.write_json(report, {'failed': 0, 'tests': [{'fullTestPath': 'UnrealAINiagara.case', 'state': 'NotRun'}]})
            with self.assertRaisesRegex(ValueError, 'did not pass'):
                suite.check_report(report, ['UnrealAINiagara'], core_tests=False)

    def test_every_extension_has_inventory_and_unique_tools(self):
        inventory = release.read(release.ROOT / 'tests/automation-inventory.json')
        ids, tools = set(), set()
        for plugin in release.components():
            metadata = release.validate(plugin)
            self.assertIn(plugin, inventory)
            self.assertNotIn(metadata['extensionId'], ids)
            ids.add(metadata['extensionId'])
            for tool in metadata['tools']:
                self.assertNotIn(tool['name'], tools)
                tools.add(tool['name'])


if __name__ == '__main__':
    unittest.main()
