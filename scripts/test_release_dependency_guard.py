"""Cases for scripts/release_dependency_guard.py: which pins a release may ship (hermetic: a fake nuget.org)."""
import json
from pathlib import Path
import sys
import tempfile
import textwrap
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_dependency_guard as guard

PUBLISHED = {
    "com.ivanmurzak.reflectornet": ["5.5.0"],
    "com.ivanmurzak.rql": ["0.1.0", "0.2.0"],
    "com.ivanmurzak.codeexec": ["0.3.0"],
}
RQL_COMMITS = {"0.1.0": "aaaa1111", "0.2.0": "b36b8e44f5952ea808af0918347774de5da69b6f"}
NUSPEC = '<package xmlns="http://schemas.microsoft.com/packaging/2013/05/nuspec.xsd"><metadata>' \
         '<id>com.IvanMurzak.Rql</id><repository type="git" commit="%s" /></metadata></package>'

BRIDGE_CSPROJ = """<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup><Version>0.25.0</Version><RqlVersion>%s</RqlVersion></PropertyGroup>
  <ItemGroup>
    <PackageReference Include="com.IvanMurzak.ReflectorNet" Version="5.5.0" />
    <PackageReference Include="com.IvanMurzak.Rql" Version="$(RqlVersion)" />
  </ItemGroup>
</Project>
"""

FEED_PROPS = """<Project>
  <PropertyGroup>
    <RestoreAdditionalProjectSources>$(RestoreAdditionalProjectSources);$(MSBuildThisFileDirectory)prerelease-packages</RestoreAdditionalProjectSources>
  </PropertyGroup>
</Project>
"""

RQL_PUBLISHED = ("- Source: https://github.com/IvanMurzak/RQL, commit `b36b8e44f5952ea808af0918347774de5da69b6f` "
                 "(`com.IvanMurzak.Rql` %s).\n")
RQL_UNRELEASED = ("- Source: https://github.com/IvanMurzak/RQL, commit `602ce7c` (main, unreleased; the\n"
                  "  sidecar pins the matching pre-release package `com.IvanMurzak.Rql` 0.3.0-dev.602ce7c).\n")


def fake_registry(published=PUBLISHED, fail=False):
    def fetch(url):
        if fail:
            raise guard.RegistryError("offline")
        if url.endswith(".nuspec"):
            package, version = url.split("/")[-3:-1]
            commit = RQL_COMMITS.get(version) if package == "com.ivanmurzak.rql" else None
            return None if commit is None else (NUSPEC % commit).encode()
        versions = published.get(url.split("/")[-2])
        return None if versions is None else json.dumps({"versions": versions}).encode()
    return guard.NuGetRegistry(fetch)


class GuardTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.fixture()

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def fixture(self, rql="0.2.0", vendored_rql=None, code_exec="0.3.0", min_core="1.0.0", feed=False):
        self.write("bridge/src/com.IvanMurzak.Unreal.MCP.Bridge.csproj", BRIDGE_CSPROJ % rql)
        self.write("bridge/tests/com.IvanMurzak.Unreal.MCP.Bridge.Tests.csproj",
                   '<Project><ItemGroup><PackageReference Include="xunit" Version="3.0.0-pre.1" /></ItemGroup></Project>')
        if feed:
            self.write("bridge/Directory.Build.props", FEED_PROPS)
        self.write(guard.RQL_VENDORED, "# RQL C++ runtime (vendored)\n\n" + (vendored_rql or RQL_PUBLISHED % rql))
        self.write(guard.CODE_EXEC_VENDORED, f"copied verbatim from Code-Exec at release **{code_exec}** (tag `nuget-v{code_exec}`).\n")
        self.write("extensions/UnrealAIGAS/extension.json", json.dumps({"version": "0.1.1", "minCoreVersion": min_core}, indent=2))
        self.write("cli/package.json", json.dumps({"dependencies": {"fflate": "^0.8.3"}}, indent=2))

    def problems(self, registry=None):
        _, problems = guard.run(self.root, registry or fake_registry())
        return [(guard.rel(p.file, self.root), p.message) for p in problems]

    def test_published_pins_pass(self):
        pins, problems = guard.run(self.root, fake_registry())
        self.assertEqual([], problems)
        self.assertEqual({"bridge/src/com.IvanMurzak.Unreal.MCP.Bridge.csproj", guard.RQL_VENDORED.as_posix()},
                         {guard.rel(p.file, self.root) for p in pins if p.package == "com.IvanMurzak.Rql"})

    def test_current_main_shape_fails_on_every_leg(self):
        self.fixture(rql="0.3.0-dev.602ce7c", vendored_rql=RQL_UNRELEASED, feed=True)
        problems = self.problems()
        self.assertIn(("bridge/src/com.IvanMurzak.Unreal.MCP.Bridge.csproj",
                       "com.IvanMurzak.Rql 0.3.0-dev.602ce7c is a pre-release version"), problems)
        self.assertIn((guard.RQL_VENDORED.as_posix(), "com.IvanMurzak.Rql 0.3.0-dev.602ce7c is a pre-release version"), problems)
        self.assertTrue(any(f == "bridge/Directory.Build.props" and "prerelease-packages" in m for f, m in problems))
        self.assertFalse(any("xunit" in m for _, m in problems))  # the test project ships nothing

    def test_vendored_rql_must_match_the_sidecar_pin_and_be_published(self):
        self.fixture(vendored_rql=RQL_PUBLISHED % "0.1.0")
        self.assertIn((guard.RQL_VENDORED.as_posix(),
                       "vendored RQL C++ runtime is com.IvanMurzak.Rql 0.1.0 but the sidecar pins 0.2.0"), self.problems())
        self.fixture(vendored_rql=RQL_PUBLISHED % "0.3.0-dev.1")
        self.assertIn((guard.RQL_VENDORED.as_posix(), "com.IvanMurzak.Rql 0.3.0-dev.1 is a pre-release version"), self.problems())
        self.fixture(vendored_rql="- Source: https://github.com/IvanMurzak/RQL, commit `abc`.\n")
        self.assertEqual([(guard.RQL_VENDORED.as_posix(),
                           "vendored com.IvanMurzak.Rql: no release version marker (copy it from a published release)")],
                         self.problems())

    def test_vendored_rql_commit_must_be_the_released_one(self):
        self.fixture(vendored_rql=(RQL_PUBLISHED % "0.2.0").replace("b36b8e44f5952ea808af0918347774de5da69b6f", "602ce7c"))
        self.assertEqual([(guard.RQL_VENDORED.as_posix(), "vendored RQL C++ runtime is commit 602ce7c but "
                           "com.IvanMurzak.Rql 0.2.0 was packed from b36b8e44f595 (copy the sources of that release)")],
                         self.problems())
        self.fixture(vendored_rql=(RQL_PUBLISHED % "0.2.0").replace("b36b8e44f5952ea808af0918347774de5da69b6f", "b36b8e4"))
        self.assertEqual([], self.problems())  # a short sha of the released commit is fine
        self.fixture(vendored_rql="- Source: RQL (`com.IvanMurzak.Rql` 0.2.0).\n")
        self.assertEqual([(guard.RQL_VENDORED.as_posix(),
                           "vendored RQL C++ runtime: no source commit (commit `<sha>`) recorded")], self.problems())

    def test_npm_unresolved_property_and_missing_file(self):
        self.write("cli/package.json", json.dumps({"dependencies": {"fflate": "next"}}))
        self.write("bridge/src/com.IvanMurzak.Unreal.MCP.Bridge.csproj", BRIDGE_CSPROJ.replace("$(RqlVersion)", "$(Nope)") % "0.2.0")
        (self.root / guard.CODE_EXEC_VENDORED).unlink()
        self.assertEqual({("cli/package.json", "fflate: 'next' is not a registry version"),
                          ("bridge/src/com.IvanMurzak.Unreal.MCP.Bridge.csproj", "com.IvanMurzak.Rql: cannot resolve version '$(Nope)'"),
                          (guard.CODE_EXEC_VENDORED.as_posix(), "expected file is missing (update release_dependency_guard.py)")},
                         set(self.problems()))

    def test_unpublished_stable_pin_fails(self):
        self.fixture(rql="0.9.0")
        problems = self.problems()
        self.assertIn(("bridge/src/com.IvanMurzak.Unreal.MCP.Bridge.csproj",
                       "com.IvanMurzak.Rql 0.9.0 is not published on nuget.org"), problems)
        self.assertIn((guard.RQL_VENDORED.as_posix(), "com.IvanMurzak.Rql 0.9.0 is not published on nuget.org"), problems)

    def test_registry_outage_fails_closed(self):
        problems = self.problems(fake_registry(fail=True))
        self.assertTrue(problems)
        self.assertTrue(all("cannot verify on nuget.org" in m or "cannot read the nuspec" in m for _, m in problems))

    def test_code_exec_and_extension_core_floor(self):
        self.fixture(code_exec="0.4.0-dev.2", min_core="1.1.0-beta")
        problems = self.problems()
        self.assertIn((guard.CODE_EXEC_VENDORED.as_posix(), "com.IvanMurzak.CodeExec 0.4.0-dev.2 is a pre-release version"), problems)
        self.assertIn(("extensions/UnrealAIGAS/extension.json",
                       "Unreal-MCP core (minCoreVersion) 1.1.0-beta is a pre-release version"), problems)

    def test_nuget_config_local_feed_fails(self):
        self.write("bridge/nuget.config", textwrap.dedent("""\
            <configuration><packageSources>
              <add key="nuget.org" value="https://api.nuget.org/v3/index.json" />
              <add key="pre" value="./prerelease-packages" />
            </packageSources></configuration>
            """))
        self.assertEqual([("bridge/nuget.config", "package source 'pre' is the local feed './prerelease-packages'")],
                         self.problems())


if __name__ == "__main__":
    unittest.main()
