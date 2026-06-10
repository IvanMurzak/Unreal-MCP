import { Command } from 'commander';
import { detectInstalledEngines, planEngineInstall } from '../lib/install-engine.js';
import * as ui from '../utils/ui.js';

export const installEngineCommand = new Command('install-engine')
  .description('Detect installed Unreal engines; for a missing version, link to the Epic launcher')
  .argument('[version]', 'Engine version to ensure (e.g. 5.7). Omit to just list installed engines.')
  .option('--manifest <path>', 'Explicit LauncherInstalled.dat path')
  .action(async (version: string | undefined, opts: { manifest?: string }) => {
    if (!version) {
      const detected = detectInstalledEngines({ manifestPath: opts.manifest });
      ui.heading('Installed Unreal engines:');
      if (detected.engines.length === 0) {
        ui.info(`  (none found${detected.manifestPath ? ` in ${detected.manifestPath}` : ''})`);
      } else {
        for (const e of detected.engines) ui.label(e.engineAssociation, `${e.appName} @ ${e.installLocation}`);
      }
      return;
    }
    const plan = planEngineInstall({ version, manifestPath: opts.manifest });
    if (plan.alreadyInstalled) {
      ui.success(plan.message);
    } else {
      ui.info(plan.message);
    }
  });
