const PACKAGE_NAME = 'unreal-mcp-cli';
const NPM_REGISTRY_URL = `https://registry.npmjs.org/${PACKAGE_NAME}/latest`;
const FETCH_TIMEOUT_MS = 3000;

/** The latest published CLI. It ships as the last leg of a plugin release, so it always names a published plugin. */
export async function fetchLatestVersion(): Promise<string> {
  const response = await fetch(NPM_REGISTRY_URL, {
    signal: AbortSignal.timeout(FETCH_TIMEOUT_MS),
    headers: { Accept: 'application/json' },
  });
  if (!response.ok) throw new Error(`npm registry returned ${response.status}`);

  const data = (await response.json()) as { version?: string };
  if (!data.version) throw new Error('No version field in registry response');
  return data.version;
}

export function isRunningViaNpx(): boolean {
  const execPath = process.env['npm_execpath'] ?? '';
  const npmCommand = process.env['npm_command'] ?? '';
  return execPath.includes('npx') || npmCommand === 'exec';
}
