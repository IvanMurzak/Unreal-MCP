// Run: node --test .github/scripts/docs-only.test.mjs

import assert from "node:assert/strict";
import { execFileSync, spawnSync } from "node:child_process";
import { mkdirSync, mkdtempSync, readFileSync, renameSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { test } from "node:test";
import { changedFiles, isDocsOnly, isDocsPath } from "./docs-only.mjs";

test("isDocsPath lists docs/ and the root AGENTS.md, nothing else", () => {
  const skip = [
    "AGENTS.md",
    "docs/ARCHITECTURE.md",
    "docs/rql.md",
    "docs/img/mcp-clients/claude-64.png",
  ];
  const run = [
    "README.md", // the source-generated tool inventory
    "cli/README.md", // npm `files`
    "extensions/README.md",
    "extensions/UnrealAINiagara/README.md", // packaged, compared by test_extension_monorepo.py, fetched by the App
    "UnrealMCP/Source/ThirdParty/RQL/README.md", // release_dependency_guard.py reads it
    "UnrealMCP/Source/ThirdParty/UnrealMcpBridge/README.md", // inside the plugin package
    "UnrealMCP/AGENTS.md",
    "bridge/AGENTS.md",
    "tests/AGENTS.md",
    "extensions/AGENTS.md",
    "extensions-index.json",
    "cli/extensions.catalog.json",
    "tests/chain-fixtures/5.8/tools.jsonl",
    "bridge/src/Program.cs",
    ".github/workflows/test_pull_request.yml",
    ".github/scripts/docs-only.mjs",
    "LICENSE",
    "docs",
    "documents/a.md",
    "Docs/a.md",
    "agents.md",
    "CHANGELOG.md",
  ];
  for (const path of skip) assert.equal(isDocsPath(path), true, path);
  for (const path of run) assert.equal(isDocsPath(path), false, path);
});

test("isDocsOnly needs at least one file and every file on the list", () => {
  assert.equal(isDocsOnly(["docs/a.md", "AGENTS.md"]), true);
  assert.equal(isDocsOnly(["docs/a.md", "cli/package.json"]), false);
  assert.equal(isDocsOnly([]), false);
});

function withRepo(check) {
  const repo = mkdtempSync(join(tmpdir(), "docs-only-"));
  const git = (...args) => execFileSync("git", ["-c", "user.name=t", "-c", "user.email=t@t", ...args], { cwd: repo, stdio: "pipe" });
  try {
    git("init", "-b", "main");
    check(repo, git);
  } finally {
    rmSync(repo, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 }); // Windows briefly keeps git's handles
  }
}

/** A repo with one base commit, then the merge commit of a branch `change` applied to it. */
function withMerge(change, check) {
  withRepo((repo, git) => {
    const write = (path) => {
      mkdirSync(dirname(join(repo, path)), { recursive: true });
      writeFileSync(join(repo, path), path);
    };
    write("bridge/a.cs");
    write("bridge/b.cs"); // keeps bridge/ alive, or git reads a rename of a.cs as moving the folder
    git("add", ".");
    git("commit", "-m", "base");
    git("checkout", "-b", "pr");
    change({ repo, write });
    git("add", ".");
    git("commit", "-m", "pr");
    git("checkout", "main");
    write("bridge/moved-on-main.cs");
    git("add", ".");
    git("commit", "-m", "main moved");
    git("merge", "--no-ff", "-m", "merge", "pr");
    check(repo);
  });
}

test("changedFiles lists only what the pull request changes, not what main gained", () => {
  withMerge(({ write }) => write("docs/a.md"), (repo) => assert.deepEqual(changedFiles(repo), ["docs/a.md"]));
});

test("a rename from code into docs lists both sides, so it is not docs-only", () => {
  withMerge(
    ({ repo }) => {
      mkdirSync(join(repo, "docs"));
      renameSync(join(repo, "bridge/a.cs"), join(repo, "docs/a.cs"));
    },
    (repo) => {
      assert.deepEqual(changedFiles(repo).sort(), ["bridge/a.cs", "docs/a.cs"]);
      assert.equal(isDocsOnly(changedFiles(repo)), false);
    },
  );
});

test("a non-ASCII docs path stays unquoted, so it still matches the list", () => {
  withMerge(({ write }) => write("docs/заметка.md"), (repo) => assert.equal(isDocsOnly(changedFiles(repo)), true));
});

test("changedFiles refuses a HEAD that is not a merge commit", () => {
  withRepo((repo, git) => {
    git("commit", "--allow-empty", "-m", "only");
    assert.throws(() => changedFiles(repo));
  });
});

test("the script writes the output only when HEAD is the commit under test", () => {
  const script = fileURLToPath(new URL("./docs-only.mjs", import.meta.url));
  withMerge(({ write }) => write("docs/a.md"), (repo) => {
    const output = join(repo, "output.txt");
    const run = (sha) => spawnSync(process.execPath, [script], { cwd: repo, encoding: "utf8", env: { ...process.env, GITHUB_OUTPUT: output, GITHUB_SHA: sha } });

    assert.notEqual(run("0".repeat(40)).status, 0);
    assert.throws(() => readFileSync(output));

    const head = execFileSync("git", ["rev-parse", "HEAD"], { cwd: repo, encoding: "utf8" }).trim();
    assert.equal(run(head).status, 0);
    assert.equal(readFileSync(output, "utf8"), "docs_only=true\n");
  });
});

/** test_pull_request.yml's job blocks by key. A block also holds the comments above the next job. */
function workflowJobs() {
  const text = readFileSync(new URL("../workflows/test_pull_request.yml", import.meta.url), "utf8").replaceAll("\r\n", "\n");
  const blocks = text.slice(text.indexOf("\njobs:\n")).split(/\n {2}(?=[\w-]+:\n)/).slice(1);
  return Object.fromEntries(blocks.map((block) => [block.slice(0, block.indexOf(":")), block]));
}

const line = (block, key) => block.match(new RegExp(`^ {4}${key}: (.*)$`, "m"))?.[1];
const FORK_GUARD = "(github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository)";

test("the gate runs only on a pull request without a train/ label, on a hosted runner", () => {
  const { changes } = workflowJobs();
  assert.equal(line(changes, "runs-on"), "ubuntu-latest");
  const condition = line(changes, "if");
  assert.ok(condition.includes("github.event_name == 'pull_request'"), condition);
  assert.ok(condition.includes("' train/'"), condition);
});

test("every job is classified: a new job must decide how it treats the gate", () => {
  assert.deepEqual(Object.keys(workflowJobs()), ["changes", "bridge", "test-cli", "plugin", "connection-smoke"]);
});

test("a heavy job skips only on an explicit true and still runs when the gate itself is skipped", () => {
  const jobs = workflowJobs();
  for (const name of ["bridge", "test-cli", "plugin"]) {
    assert.equal(line(jobs[name], "needs"), "changes", name);
    assert.ok(line(jobs[name], "if").includes("!cancelled() && needs.changes.outputs.docs_only != 'true'"), name);
  }
  assert.ok(line(jobs.plugin, "if").includes("vars.UNREAL_RUNNER_READY == 'true'"));
  assert.ok(line(jobs.plugin, "if").includes(FORK_GUARD));
});

test("the smoke follows the plugin job's result, because a skipped gate would skip it through the implicit success()", () => {
  const smoke = workflowJobs()["connection-smoke"];
  assert.equal(line(smoke, "needs"), "[plugin]");
  const condition = line(smoke, "if");
  assert.ok(condition.includes("!cancelled() && needs.plugin.result == 'success'"), condition);
  assert.ok(condition.includes("vars.UNREAL_SMOKE_READY == 'true'"), condition);
  assert.ok(condition.includes(FORK_GUARD), condition);
});
