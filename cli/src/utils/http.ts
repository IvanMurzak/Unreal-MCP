// Shared HTTP helper: `fetch` with a per-request abort deadline.
//
// Three call sites (login's device-flow polls, the readiness `probe`, and
// `run-tool`'s tool invocation) each hand-rolled the same AbortController +
// setTimeout dance. Centralise it here. The helper re-throws on failure (it
// does NOT classify the error) so each caller keeps its own error mapping —
// login/probe rely on the thrown `AbortError`'s `name`, while run-tool needs
// to tell its OWN timeout apart from a caller-supplied cancellation, which the
// `onTimeout` callback surfaces.

/** Options for {@link fetchWithTimeout}. */
export interface FetchWithTimeoutOptions {
  /** Per-request abort deadline in milliseconds. */
  timeoutMs: number;
  /**
   * A caller-supplied abort signal (e.g. a CLI cancellation). When it fires,
   * the request is aborted WITHOUT invoking `onTimeout` — so the caller can
   * distinguish a deliberate cancellation from a timeout. An already-aborted
   * signal aborts the request synchronously before the fetch starts.
   */
  externalSignal?: AbortSignal;
  /**
   * Invoked exactly when the internal timeout fires (NOT on an external-signal
   * abort). Lets a caller flag "this abort was my timeout" so its catch block
   * can map the resulting `AbortError` to a timeout rather than a cancellation.
   */
  onTimeout?: () => void;
}

/**
 * `fetch` with a per-request abort deadline so a hung endpoint cannot stall a
 * flow indefinitely. Honors the injected `fetchImpl` (for tests). On the
 * deadline OR an `externalSignal` abort the underlying request is aborted and
 * `fetchImpl` rejects with an `AbortError`, which propagates to the caller
 * unchanged (the helper does not catch it). The timeout timer is always
 * cleared and the external-signal listener always removed.
 */
export async function fetchWithTimeout(
  fetchImpl: typeof fetch,
  url: string,
  init: RequestInit,
  opts: FetchWithTimeoutOptions,
): Promise<Response> {
  const controller = new AbortController();
  const timer = setTimeout(() => {
    opts.onTimeout?.();
    controller.abort();
  }, opts.timeoutMs);

  const externalAbort = (): void => controller.abort();
  const externalSignal = opts.externalSignal;
  if (externalSignal) {
    if (externalSignal.aborted) controller.abort();
    else externalSignal.addEventListener('abort', externalAbort, { once: true });
  }

  try {
    return await fetchImpl(url, { ...init, signal: controller.signal });
  } finally {
    clearTimeout(timer);
    externalSignal?.removeEventListener('abort', externalAbort);
  }
}
