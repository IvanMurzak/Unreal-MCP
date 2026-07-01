// Error coercion helper shared across the CLI's library-safe boundaries.
//
// Every `lib/*` entry point returns errors rather than throwing, which means a
// caught `unknown` must be normalised to a real `Error` (so `.message` /
// `.name` are always available). This was open-coded as
// `err instanceof Error ? err : new Error(String(err))` in ~14 files; centralise
// it here so the coercion is written (and tested) once. Pure.

/**
 * Coerce an unknown caught value into an `Error`. Returns the value unchanged
 * when it already is an `Error`; otherwise wraps its `String(...)` form in a
 * fresh `Error`. Behaviour-identical to the inline
 * `err instanceof Error ? err : new Error(String(err))` it replaces — use
 * `asError(err).message` where only the message string is needed.
 */
export function asError(err: unknown): Error {
  return err instanceof Error ? err : new Error(String(err));
}
