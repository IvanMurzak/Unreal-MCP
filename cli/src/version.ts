// Single source of the package version at runtime: read from package.json so
// `commands/bump-version.ps1` only has to rewrite one file in cli/.
import { createRequire } from 'module';

const require = createRequire(import.meta.url);
const pkg = require('../package.json') as { name: string; version: string };

export const PACKAGE_NAME: string = pkg.name;
export const PACKAGE_VERSION: string = pkg.version;
