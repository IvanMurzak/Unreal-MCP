import { describe, it, expect } from 'vitest';
import { Command } from 'commander';
import { configureStyledHelp } from '../src/utils/ui.js';
import { PACKAGE_VERSION } from '../src/version.js';

describe('configureStyledHelp', () => {
  it('renders the Unreal banner on the root command', () => {
    const program = new Command().name('unreal-mcp-cli').description('root');
    configureStyledHelp(program, PACKAGE_VERSION);
    const help = program.helpInformation();
    expect(help).toContain('Unreal-MCP CLI');
    expect(help).toContain(`v${PACKAGE_VERSION}`);
    expect(help).toContain('unreal-mcp-cli <command> --help');
  });
});
