import type { ProgressCallback, ProgressEvent } from './types.js';

/** Safely emit a progress event — a throwing callback never escapes. */
export function emitProgress(cb: ProgressCallback | undefined, event: ProgressEvent): void {
  if (!cb) return;
  try {
    cb(event);
  } catch {
    /* a consumer's onProgress must never break the library call */
  }
}

/** ` (PID: n)` when the pid is known, else nothing: a log line never says "PID: unknown". */
export function pidSuffix(pid: number | undefined): string {
  return pid !== undefined ? ` (PID: ${pid})` : '';
}
