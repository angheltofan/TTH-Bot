// Minimal assertions, so the test suite has no external dependencies.

export function assert(condition: unknown, message = "assertion failed"): asserts condition {
  if (!condition) throw new Error(message);
}

function show(value: unknown): string {
  if (value instanceof Uint8Array) return `Uint8Array[${[...value].join(",")}]`;
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

function deepEqual(a: unknown, b: unknown): boolean {
  if (Object.is(a, b)) return true;
  if (a instanceof Uint8Array && b instanceof Uint8Array) {
    return a.length === b.length && a.every((v, i) => v === b[i]);
  }
  if (typeof a !== "object" || typeof b !== "object" || a === null || b === null) {
    return false;
  }
  if (Array.isArray(a) !== Array.isArray(b)) return false;
  const ka = Object.keys(a as object);
  const kb = Object.keys(b as object);
  if (ka.length !== kb.length) return false;
  return ka.every((k) =>
    deepEqual((a as Record<string, unknown>)[k], (b as Record<string, unknown>)[k])
  );
}

export function assertEquals<T>(actual: T, expected: T, message?: string): void {
  if (!deepEqual(actual, expected)) {
    throw new Error(
      `${message ?? "not equal"}\n  actual:   ${show(actual)}\n  expected: ${show(expected)}`,
    );
  }
}

export function assertThrows(fn: () => unknown, message = "expected a throw"): void {
  try {
    fn();
  } catch {
    return;
  }
  throw new Error(message);
}

export async function assertRejects(
  fn: () => Promise<unknown>,
  message = "expected a rejection",
): Promise<void> {
  try {
    await fn();
  } catch {
    return;
  }
  throw new Error(message);
}
