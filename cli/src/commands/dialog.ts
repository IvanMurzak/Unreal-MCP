import { Command } from 'commander';
import { findProjectDialogs, NO_DIALOG_HINT, pressEditorDialogButton } from '../lib/editor-dialogs.js';
import { ExitCode } from '../utils/exit-codes.js';
import { CommandOutput, FAILURE, formatOption } from '../utils/output.js';
import * as ui from '../utils/ui.js';

interface DialogCliOptions {
  path?: string;
  press?: string;
  id?: string;
  format: string;
}

export const dialogCommand = new Command('dialog')
  .description('List the modal dialogs blocking the project\'s Unreal Editor, or press a button on one (served by the UnrealMCP plugin; any OS, any connection mode)')
  .argument('[path]', 'Unreal project directory (default: current directory)')
  .option('-p, --path <dir>', 'Unreal project directory')
  .option('--press <button>', 'Press the button with this label (case-insensitive)')
  .option('--id <id>', 'With --press: the dialog to answer (its id from the list); default: the first dialog with that button')
  .addOption(formatOption())
  .action(async (positionalPath: string | undefined, options: DialogCliOptions) => {
    const out = new CommandOutput('dialog', options.format);
    if (options.id !== undefined && options.press === undefined) {
      out.fail(FAILURE.usage, '--id needs --press');
      return;
    }
    if (options.id !== undefined && !/^\d+$/.test(options.id)) {
      out.fail(FAILURE.usage, `--id must be a dialog id from the list (got: "${options.id}")`);
      return;
    }
    const projectDir = options.path ?? positionalPath ?? process.cwd();
    const found = findProjectDialogs(projectDir);
    if (!found) {
      out.fail(FAILURE.projectNotFound, `No Unreal project (.uproject) at ${projectDir}`);
      return;
    }
    if (found.pids.length === 0) {
      out.fail(FAILURE.unreachable, 'No Unreal Editor has this project open.');
      return;
    }

    if (options.press !== undefined) {
      const pressed = await pressEditorDialogButton(found.project.projectDir, found.pids, options.press, options.id);
      if (pressed.kind !== 'pressed') {
        out.fail(pressed.kind === 'not-found' ? FAILURE.notFound : FAILURE.timeout, pressed.message, { pids: found.pids, dialogs: found.dialogs });
        return;
      }
      if (!out.machine) ui.success(`Pressed "${pressed.button}" on "${pressed.dialog.title}" (PID ${pressed.dialog.pid}).`);
      out.finish(ExitCode.Success, { pid: pressed.dialog.pid, pressed: pressed.button, dialog: pressed.dialog });
      return;
    }

    if (!out.machine) {
      if (found.dialogs.length === 0) {
        ui.success(`No modal dialog blocks the editor (PID ${found.pids.join(', ')}).`);
        ui.info(NO_DIALOG_HINT);
      }
      for (const d of found.dialogs) {
        ui.warn(`"${d.title}" (id ${d.id}, PID ${d.pid})`);
        if (d.message) console.log(`  ${d.message.split('\n').join('\n  ')}`);
        console.log(`  Buttons: ${d.buttons.length > 0 ? d.buttons.map((b) => `[${b}]`).join(' ') : '(none with a label)'}`);
      }
    }
    out.finish(ExitCode.Success, { pids: found.pids, dialogs: found.dialogs });
  });
