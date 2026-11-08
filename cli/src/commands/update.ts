import { Command } from 'commander';
import { spawn } from 'child_process';
import * as path from 'path';
import { platform } from 'os';
import { compareSemver } from '../lib/install-extension.js';
import { findUnrealProjectRoot, updateProject, type ProjectUpdateItem } from '../lib/update.js';
import { findUProjectFile } from '../utils/project.js';
import * as ui from '../utils/ui.js';
import { fetchLatestVersion, isRunningViaNpx } from '../utils/update-check.js';
import { PACKAGE_VERSION } from '../version.js';

const PACKAGE_NAME = 'unreal-mcp-cli';

/** Exit 2 with `--check`: something is outdated (distinct from exit 1 = error). */
const EXIT_OUTDATED = 2;

interface UpdateCommandOptions {
  path?: string;
  pluginSource?: string;
  version?: string;
  force?: boolean;
  check?: boolean;
  /** Commander maps each `--no-x` to `x` being `false`. */
  clean?: boolean;
  self?: boolean;
  project?: boolean;
  extensions?: boolean;
}

type Outcome = 'done' | 'outdated' | 'failed';

function runNpmInstall(packageSpec: string): Promise<number> {
  const isWindows = platform() === 'win32';
  return new Promise((resolve, reject) => {
    // Node refuses to spawn a `.cmd` without a shell (EINVAL). The version in the spec is checked first.
    const child = spawn(isWindows ? 'npm.cmd' : 'npm', ['install', '-g', packageSpec], { stdio: 'inherit', shell: isWindows });
    child.on('error', reject);
    child.on('close', (code) => resolve(code ?? 1));
  });
}

function printItem(item: ProjectUpdateItem): void {
  const from = item.from ?? 'unknown';
  const value =
    item.status === 'outdated' ? `${from} → ${item.to ?? 'latest'}`
    : item.status === 'unknown' ? `${from} (latest version unknown)`
    : `${from} (up to date)`;
  ui.label(item.name, value);
}

async function updateProjectStep(projectDir: string, options: UpdateCommandOptions): Promise<Outcome> {
  ui.heading(`Project ${projectDir}`);
  const spinner = ui.startSpinner(options.check ? 'Checking the plugin and extensions...' : 'Updating the plugin and extensions...');
  const result = await updateProject({
    projectDir,
    pluginSourceDir: options.pluginSource,
    version: options.version,
    force: options.force,
    noClean: options.clean === false,
    noExtensions: options.extensions === false,
    dryRun: options.check,
    onProgress: (event) => {
      spinner.text = event.message;
    },
  });
  if (result.kind === 'failure') {
    spinner.error(result.error.message);
    ui.printWarnings(result.warnings);
    return 'failed';
  }

  const outdated = result.items.filter((item) => item.status === 'outdated');
  spinner.success(options.check ? `${outdated.length} update(s) available` : `${result.updated.length} updated`);
  result.items.forEach(printItem);
  ui.printWarnings(result.warnings);
  for (const step of result.nextSteps) ui.info(step);

  if (result.items.length === 0) {
    ui.info('UnrealMCP is not installed in this project (a Fab install is updated by the Epic Games Launcher): nothing to update here.');
  }
  if (options.check) return outdated.length > 0 ? 'outdated' : 'done';
  return result.updated.length < outdated.length ? 'failed' : 'done';
}

async function updateSelfStep(check: boolean | undefined): Promise<Outcome> {
  if (isRunningViaNpx()) {
    ui.info('Running via npx — it always fetches the latest published version.');
    return 'done';
  }

  const spinner = ui.startSpinner(`Checking for a newer ${PACKAGE_NAME}...`);
  let latest: string;
  try {
    latest = await fetchLatestVersion();
  } catch (err) {
    spinner.error(`Could not check for a newer ${PACKAGE_NAME}: ${(err as Error).message}`);
    return 'failed';
  }
  if (!/^\d+\.\d+\.\d+[0-9A-Za-z.+-]*$/.test(latest)) {
    spinner.error(`The registry returned an unexpected version: ${latest}`);
    return 'failed';
  }
  if (compareSemver(PACKAGE_VERSION, latest) >= 0) {
    spinner.success(`${PACKAGE_NAME} is up to date (v${PACKAGE_VERSION})`);
    return 'done';
  }

  spinner.success(`${PACKAGE_NAME} ${PACKAGE_VERSION} → ${latest}`);
  if (check) return 'outdated';

  try {
    const exitCode = await runNpmInstall(`${PACKAGE_NAME}@${latest}`);
    if (exitCode !== 0) {
      ui.error(`Update failed. Try: ${platform() === 'win32' ? '' : 'sudo '}npm install -g ${PACKAGE_NAME}@latest`);
      return 'failed';
    }
  } catch (err) {
    ui.error(`Could not run npm: ${(err as Error).message}. Make sure npm is installed and in your PATH.`);
    return 'failed';
  }
  ui.success(`Updated ${PACKAGE_NAME} to v${latest}`);
  return 'done';
}

/** The project to update, or null when there is none around the current folder. A named path that is not one exits 1. */
function resolveProject(explicit: string | undefined): string | null {
  if (explicit === undefined) return findUnrealProjectRoot(process.cwd());

  const projectDir = path.resolve(explicit);
  if (findUProjectFile(projectDir) === null) {
    ui.error(`Not an Unreal project (no .uproject): ${projectDir}`);
    process.exit(1);
  }
  return projectDir;
}

export const updateCommand = new Command('update')
  .description(`Update everything: the Unreal project's UnrealMCP plugin and installed extensions to the latest version, then ${PACKAGE_NAME} itself`)
  .argument('[path]', 'Unreal project directory (defaults to the project around the current directory)')
  .option('--path <path>', 'Unreal project directory')
  .option('--plugin-source <dir>', 'UnrealMCP plugin source dir (offline / CI / dev override)')
  .option('--version <v>', 'Plugin-source release version to download (defaults to the latest release)')
  .option('--force', 'Re-install the plugin even when versions match')
  .option('--check', 'Only report what would change. Exits 2 when something is outdated')
  .option('--no-self', `Leave ${PACKAGE_NAME} itself as it is`)
  .option('--no-project', 'Leave the project as it is: update only the CLI')
  .option('--no-extensions', 'Leave the installed extensions as they are')
  .option(
    '--no-clean',
    'Do not wipe the stale UE C++ build cache after a copy-mode update (cleaning is the default; the bundled sidecar bridge is always preserved)',
  )
  .action(async (pathArg: string | undefined, options: UpdateCommandOptions) => {
    const outcomes: Outcome[] = [];

    if (options.project !== false) {
      const projectDir = resolveProject(pathArg ?? options.path);
      if (projectDir === null) {
        ui.info('No Unreal project here, so only the CLI is updated. Pass a project path to update its plugin and extensions.');
      } else {
        outcomes.push(await updateProjectStep(projectDir, options));
      }
    }

    // Last: npm replaces this very process's files, and the project step does not need a newer CLI
    // because it looks the latest versions up itself.
    if (options.self !== false) outcomes.push(await updateSelfStep(options.check));

    process.exit(outcomes.includes('failed') ? 1 : outcomes.includes('outdated') ? EXIT_OUTDATED : 0);
  });
