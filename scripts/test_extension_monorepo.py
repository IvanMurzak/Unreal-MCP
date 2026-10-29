import copy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import extension_release as release
import extension_suite as suite


class MonorepoTests(unittest.TestCase):
    def test_bad_uploaded_archive_never_promotes_index_and_draft_retry_recovers(self):
        class FakeGitHub:
            def __init__(self):
                self.index = release.read(release.ROOT / 'extensions-index.json')
                self.releases = {}
                self.index_writes = 0
                self.corrupt_download = True

            def __call__(self, *args):
                if args[0] == 'api':
                    return 'sha' if 'commits/HEAD' in args[1] else json.dumps(list(self.releases.values()))
                verb, tag = args[1:3]
                if verb == 'create':
                    self.releases[tag] = {'tag_name': tag, 'draft': True, 'bytes': Path(args[-1]).read_bytes().hex()}
                elif verb == 'download':
                    output = Path(args[args.index('--dir') + 1])
                    name = args[args.index('--pattern') + 1]
                    if tag == release.INDEX_TAG:
                        release.write(output / name, self.index)
                    else:
                        data = b'corrupted' if self.corrupt_download else bytes.fromhex(self.releases[tag]['bytes'])
                        (output / name).write_bytes(data)
                elif verb == 'edit':
                    self.releases[tag]['draft'] = False
                elif verb == 'upload':
                    if tag == release.INDEX_TAG:
                        self.index = release.read(Path(args[3]))
                        self.index_writes += 1
                    else:
                        self.releases[tag]['bytes'] = Path(args[3]).read_bytes().hex()
                else:
                    raise AssertionError(args)
                return ''

        github = FakeGitHub()
        original = copy.deepcopy(github.index)
        version = release.validate('UnrealAINiagara')['version']
        tag = f'niagara-v{version}'
        with tempfile.TemporaryDirectory() as temporary:
            archive = release.package('UnrealAINiagara', Path(temporary))
            with patch.object(release, 'gh', github), patch.object(release.subprocess, 'check_output', return_value='sha\n'):
                with self.assertRaisesRegex(ValueError, 'Uploaded archive differs'):
                    release.publish('UnrealAINiagara', archive)
                self.assertEqual(github.index_writes, 0)
                self.assertEqual(github.index, original)
                self.assertTrue(github.releases[tag]['draft'])
                github.corrupt_download = False
                release.publish('UnrealAINiagara', archive)
                self.assertEqual(github.index_writes, 1)
                self.assertFalse(github.releases[tag]['draft'])
                entry = next(e for e in github.index['extensions'] if e['pluginName'] == 'UnrealAINiagara')
                self.assertEqual(entry['releases'][version]['tag'], tag)
                # A full retry retains the initial workflow's bootstrap input.
                # The now-live index must be downloaded, preserving all history.
                with patch.object(release.subprocess, 'run') as probe:
                    probe.return_value.returncode = 0
                    release.publish('UnrealAINiagara', archive, bootstrap=True)
                self.assertEqual(github.index_writes, 2)
                self.assertIn('0.1.0', next(e for e in github.index['extensions']
                                          if e['pluginName'] == 'UnrealAINiagara')['releases'])
                with patch.object(release.subprocess, 'run') as probe:
                    probe.return_value.returncode = 1
                    probe.return_value.stderr = b'HTTP 503: temporarily unavailable'
                    with self.assertRaisesRegex(ValueError, 'Cannot determine'):
                        release.publish('UnrealAINiagara', archive, bootstrap=True)
                self.assertEqual(github.index_writes, 2)

    def test_misnested_new_extension_cannot_silently_escape_discovery(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'extensions/UnrealAINew').mkdir(parents=True)
            release.write(root / 'extensions/UnrealAINew/extension.json', {'pluginName': 'UnrealAINew'})
            with patch.object(release, 'ROOT', root), self.assertRaisesRegex(ValueError, 'no matching top-level plugin'):
                release.components()

    def test_default_output_is_absolute(self):
        self.assertTrue(suite.default_output().is_absolute())

    def test_smoke_host_syncs_the_template_and_leaves_unchanged_files_alone(self):
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            with patch.object(suite, 'default_output', return_value=out):
                project = suite.smoke_host()
                self.assertEqual(project, out / 'smoke-host/UnrealTestProject/UnrealTestProject.uproject')
                template = suite.ROOT / 'tests/smoke-host'
                expected = {f.relative_to(template) for f in template.rglob('*') if f.is_file()}
                self.assertEqual({f.relative_to(project.parent) for f in project.parent.rglob('*') if f.is_file()}, expected)
                target = project.parent / 'Source/UnrealTestProjectEditor.Target.cs'
                stamp = target.stat().st_mtime_ns
                (project.parent / 'Config/DefaultGame.ini').write_text('drift')
                suite.smoke_host()
                self.assertEqual(target.stat().st_mtime_ns, stamp, 'an unchanged Target.cs must not be rewritten')
                self.assertEqual((project.parent / 'Config/DefaultGame.ini').read_bytes(), (template / 'Config/DefaultGame.ini').read_bytes())

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
                self.assertEqual(archive.read('UnrealAINiagara/README.md'),
                                 (release.ROOT / 'extensions/UnrealAINiagara/README.md').read_bytes())
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
        tag = f"niagara-v{meta['version']}"
        index = release.update_index(index, meta, tag, 'a' * 64)
        self.assertEqual(index, release.update_index(index, meta, tag, 'a' * 64))
        with self.assertRaisesRegex(ValueError, 'immutable'):
            release.update_index(index, meta, tag, 'b' * 64)

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

    def test_unchanged_profile_does_not_invalidate_ubt_caches(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            engine = root / 'engine'
            suite.write_json(engine / 'Engine/Build/Build.version', {'MajorVersion': 5, 'MinorVersion': 8})
            project = suite.prepare(engine, 'all', ['UnrealAINiagara'], root / 'out', core_tests=True)
            files = [p for p in project.parent.rglob('*') if p.is_file()]
            stamps = {p: p.stat().st_mtime_ns for p in files}
            suite.prepare(engine, 'all', ['UnrealAINiagara'], root / 'out', core_tests=True)
            self.assertEqual(stamps, {p: p.stat().st_mtime_ns for p in files})
            descriptor = suite.extensions()['UnrealAINiagara'] / 'UnrealAINiagara.uplugin'
            copied = project.parent / 'Plugins/UnrealAINiagara/UnrealAINiagara.uplugin'
            self.assertEqual(descriptor.read_bytes(), copied.read_bytes())

    def test_report_gate_catches_missing_extension_and_missing_individual_test(self):
        with tempfile.TemporaryDirectory() as temporary:
            report = Path(temporary) / 'index.json'
            minimum = release.read(release.ROOT / 'tests/automation-inventory.json')['UnrealAINiagara']
            tests = [{'fullTestPath': f'UnrealAINiagara.case{i}', 'state': 'Success'} for i in range(minimum)]
            suite.write_json(report, {'failed': 0, 'tests': tests})
            self.assertEqual(suite.check_report(report, ['UnrealAINiagara'], core_tests=False)['passed'], minimum)
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

    def test_migrated_catalog_names_remain_valid_cli_install_aliases(self):
        seed = release.read(release.ROOT / 'extensions-index.json')
        for entry in seed['extensions']:
            metadata = release.validate(entry['pluginName'])
            self.assertEqual(metadata['extensionId'], entry['extensionId'])
            self.assertEqual(metadata['name'], entry['name'])


if __name__ == '__main__':
    unittest.main()
