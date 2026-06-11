import { Command } from 'commander';
import { fileURLToPath } from 'url';
import * as path from 'path';
import { bootstrapLocal } from '../lib/bootstrap-local.js';
import { findRepoRoot } from '../utils/repo.js';
import * as ui from '../utils/ui.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));

export const bootstrapLocalCommand = new Command('bootstrap-local')
  .description('Build the bridge from source into <project>/Intermediate/UnrealMCP (§6); the MCP server is downloaded from GameDev-MCP-Server releases, not built here')
  .argument('[path]', 'Unreal project directory (defaults to cwd)')
  .option('--repo-root <dir>', 'Unreal-MCP repo root (defaults to the CLI\'s repo)')
  .option('--plan', 'Print the build plan without running dotnet')
  .action(async (pathArg: string | undefined, opts: { repoRoot?: string; plan?: boolean }) => {
    const projectDir = pathArg ?? process.cwd();
    const repoRoot = opts.repoRoot ?? findRepoRoot(HERE);
    if (!repoRoot) {
      ui.error('Could not locate the Unreal-MCP repo root. Pass --repo-root <dir>.');
      process.exitCode = 1;
      return;
    }
    const result = await bootstrapLocal({
      projectDir,
      repoRoot,
      // --plan: don't actually build, just report the steps.
      buildImpl: opts.plan ? async () => {} : undefined,
      // Suppress the per-step "Building/Published" progress under --plan —
      // nothing is built, so printing build progress would be misleading.
      onProgress: opts.plan
        ? undefined
        : (e) => {
            if (e.phase === 'info' || e.phase === 'file-written') ui.info(e.message);
          },
    });
    if (result.kind === 'failure') {
      ui.printWarnings(result.warnings);
      ui.error(result.error.message);
      process.exitCode = 1;
      return;
    }
    if (opts.plan) {
      ui.heading('Build plan:');
      for (const s of result.steps) ui.label(s.label, `${s.projectFile} -> ${s.outputDir} (${s.rid})`);
    } else {
      ui.success(`Bootstrapped bridge into ${result.outputRoot}`);
      // A bootstrapped build writes no `version` file, so the plugin's §6
      // version-match would treat it as mismatched. The supported way to
      // consume a local build is the UNREAL_MCP_BRIDGE_PATH dev override,
      // which skips the download + version check entirely.
      ui.info(
        `Set UNREAL_MCP_BRIDGE_PATH to the bridge binary under ${result.outputRoot} to use this local build (skips download + version check, §6).`,
      );
      ui.info(
        'The MCP server is not built here — `setup-mcp <agent> --transport stdio` downloads the shared gamedev-mcp-server release (or set UNREAL_MCP_SERVER_PATH to a local build from the GameDev-MCP-Server repo).',
      );
    }
  });
