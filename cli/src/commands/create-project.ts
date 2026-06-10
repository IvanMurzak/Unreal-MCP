import { Command } from 'commander';
import { createProject } from '../lib/create-project.js';
import * as ui from '../utils/ui.js';

export const createProjectCommand = new Command('create-project')
  .description('Scaffold a minimal Unreal Engine C++ project')
  .argument('<path>', 'Directory to create the project in')
  .option('--name <name>', 'Project/module name (defaults to directory name; no hyphens)')
  .option('--engine <version>', 'EngineAssociation to bake in (e.g. 5.7)')
  .option('--force', 'Scaffold into a non-empty directory')
  .action(async (pathArg: string, opts: { name?: string; engine?: string; force?: boolean }) => {
    const result = await createProject({
      projectDir: pathArg,
      projectName: opts.name,
      engineAssociation: opts.engine,
      force: opts.force,
    });
    ui.printWarnings(result.warnings);
    if (result.kind === 'failure') {
      ui.error(result.errorMessage);
      process.exitCode = 1;
      return;
    }
    ui.success(`Scaffolded ${result.projectName} (${result.filesWritten.length} files)`);
    ui.label('uproject', result.uprojectPath);
  });
