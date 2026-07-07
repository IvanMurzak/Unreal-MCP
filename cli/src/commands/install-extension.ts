import { Command } from 'commander';
import * as path from 'path';
import { installExtension } from '../lib/install-extension.js';
import * as ui from '../utils/ui.js';

export const installExtensionCommand = new Command('install-extension')
  .description('Install an Unreal-MCP extension plugin into a project: place it under Plugins/, enable it + its gating engine plugins in the .uproject, then compile (on next editor open, or now with --build). Idempotent.')
  .argument('<id>', 'Extension id to install (the catalog extensionId / name / pluginName)')
  .argument('[path]', 'Unreal project directory (defaults to cwd)')
  .option('--source <dir>', 'Install from a local plugin directory (offline / CI / dev) instead of downloading')
  .option('--version <x.y.z>', 'Override the catalog-pinned version to install (also the GitHub release version)')
  .option('--build', 'Compile the project via UnrealBuildTool now (Windows-only; default: recompile on next editor open)')
  .option('--engine-root <dir>', 'Engine root for --build (default: resolved from the project EngineAssociation)')
  .option('--force', 'Re-materialize + re-enable even when already up to date')
  .action(
    async (
      id: string,
      pathArg: string | undefined,
      opts: { source?: string; version?: string; build?: boolean; engineRoot?: string; force?: boolean },
    ) => {
      const projectDir = path.resolve(pathArg ?? process.cwd());
      const spinner = ui.startSpinner(`Installing extension ${id}...`);

      const result = await installExtension({
        projectDir,
        extensionId: id,
        source: opts.source,
        version: opts.version,
        build: opts.build,
        engineRoot: opts.engineRoot,
        force: opts.force,
        onProgress: (event) => {
          spinner.text = event.message;
        },
      });

      if (result.kind === 'failure') {
        spinner.error(result.error.message);
        ui.printWarnings(result.warnings);
        process.exitCode = 1;
        return;
      }

      spinner.success(result.message);
      ui.printWarnings(result.warnings);
      ui.label('Installed at', result.installedPath);
      if (result.enabledPlugins.length > 0) {
        ui.label('Enabled plugins', result.enabledPlugins.join(', '));
      }
      if (result.rebuildRequired) {
        ui.label('Next step', 'Open the project (or re-run with --build) to compile the extension.');
      }
    },
  );
