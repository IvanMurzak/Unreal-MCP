import { Command } from 'commander';
import { detectInstalledEngines, planEngineInstall } from '../lib/install-engine.js';
import { autoInstallEngine } from '../lib/auto-install-engine.js';
import { discoverEngine } from '../lib/engine.js';
import * as ui from '../utils/ui.js';

export const installEngineCommand = new Command('install-engine')
  .description('Detect installed Unreal engines; for a missing version, link to the Epic launcher')
  .argument('[version]', 'Engine version to ensure (e.g. 5.7). Omit to just list installed engines.')
  .option('--manifest <path>', 'Explicit LauncherInstalled.dat path')
  .option(
    '--install',
    'Best-effort, consent-gated engine acquisition (no unattended installer exists today — guides you through Epic)',
  )
  .option('--yes', 'Grant consent for the multi-GB engine install spend (alias of confirming --install)')
  .action(async (version: string | undefined, opts: { manifest?: string; install?: boolean; yes?: boolean }) => {
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

    // Best-effort, consent-gated auto-install path (the brief's "include
    // auto-install"). Resolution is validated through the full discovery chain
    // so we never claim an engine is present unless a real binary resolves.
    if (opts.install || opts.yes) {
      const result = autoInstallEngine({
        version,
        consent: opts.yes === true || opts.install === true,
        interactive: process.stdout.isTTY === true,
        resolveInstalledImpl: (v) => {
          const r = discoverEngine({ engineAssociation: v, noCache: true });
          return r.kind === 'resolved' ? r.editorPath : null;
        },
      });
      if (result.outcome === 'already-installed') {
        ui.success(result.message);
        return;
      }
      ui.info(result.message);
      ui.heading('How to install it:');
      for (const step of result.guidance) ui.info(`  ${step}`);
      if (result.outcome === 'consent-required') process.exitCode = 1;
      return;
    }

    const plan = planEngineInstall({ version, manifestPath: opts.manifest });
    if (plan.alreadyInstalled) {
      ui.success(plan.message);
    } else {
      ui.info(plan.message);
    }
  });
