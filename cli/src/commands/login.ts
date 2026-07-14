import { Command } from 'commander';
import { login } from '../lib/login.js';
import * as ui from '../utils/ui.js';

export const loginCommand = new Command('login')
  .description(
    'Authorize against ai-game.dev via the OAuth device-code flow. Signs in once ' +
      'per machine: the credential is saved to the shared machine store ' +
      '(~/.ai-game-dev/credentials.json), never a project file.',
  )
  .option('-p, --path <dir>', 'Persist the credential into this project\'s .env (gitignored) instead of the shared machine store')
  .option('--base-url <url>', 'Auth base URL (defaults to https://ai-game.dev)')
  .option('--timeout <ms>', 'Overall poll timeout in ms', (v) => parseInt(v, 10))
  .action(async (opts: { path?: string; baseUrl?: string; timeout?: number }) => {
    const spinner = ui.startSpinner('Starting device-code login...');
    const result = await login({
      projectDir: opts.path,
      baseUrl: opts.baseUrl,
      timeoutMs: opts.timeout,
      onProgress: (e) => {
        if (e.phase === 'info' || e.phase === 'start') spinner.text = e.message;
      },
    });
    if (result.kind === 'failure') {
      spinner.error(`Login failed (${result.reason}): ${result.error.message}`);
      process.exitCode = 1;
      return;
    }
    spinner.success('Logged in.');
    if (result.persistedTo === 'project-env') {
      ui.info(`Credential saved to project ${result.credentialPath}`);
    } else {
      ui.info(`Signed in once for this machine — credential saved to ${result.credentialPath}`);
    }
  });
