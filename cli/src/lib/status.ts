// Side-effect-free library implementation backing the `status` command.
import { PACKAGE_NAME, PACKAGE_VERSION } from '../version.js';

export interface CliStatus {
  kind: 'success';
  success: true;
  name: string;
  version: string;
  stage: 'pre-alpha scaffold';
}

export function getStatus(): CliStatus {
  return {
    kind: 'success',
    success: true,
    name: PACKAGE_NAME,
    version: PACKAGE_VERSION,
    stage: 'pre-alpha scaffold',
  };
}
