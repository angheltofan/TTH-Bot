// Structured logging with an ALLOW-LIST of fields (PHASE6_PLAN §4.7).
//
// Only the keys below ever reach the output; everything else is dropped. So
// audio, prompt text, participant (child) names, device tokens, Gemini tokens
// and secrets cannot be logged by accident — there is no key under which they
// would survive. String values are further restricted to a safe character set
// and 64 characters.

export type LogValue = string | number | boolean;
export type LogFields = Record<string, LogValue | undefined>;

const STRING_KEYS = new Set([
  "device", // registry id, e.g. "core2-01" — not a person
  "session",
  "activity", // activity UUID, never its title or prompt
  "code",
  "reason",
  "mode",
  "fw",
]);
const NUMBER_KEYS = new Set([
  "turn",
  "frames",
  "bytes",
  "credit",
  "ms",
  "count",
  "status",
  "stale",
  "purged",
  "received",
  "sent",
  "outstanding",
  "limit",
]);
const BOOLEAN_KEYS = new Set(["retry", "converted"]);

const UNSAFE = /[^A-Za-z0-9_.:\-]/g;

function sanitize(value: string): string {
  return value.replace(UNSAFE, "?").slice(0, 64);
}

export interface Logger {
  info(event: string, fields?: LogFields): void;
  warn(event: string, fields?: LogFields): void;
  error(event: string, fields?: LogFields): void;
}

export function createLogger(
  sink: (line: string) => void = (line) => console.log(line),
  now: () => string = () => new Date().toISOString(),
): Logger {
  const write = (level: string, event: string, fields?: LogFields) => {
    const out: Record<string, LogValue> = {
      ts: now(),
      level,
      event: sanitize(event),
    };
    if (fields) {
      for (const [key, value] of Object.entries(fields)) {
        if (typeof value === "string" && STRING_KEYS.has(key)) {
          out[key] = sanitize(value);
        } else if (
          typeof value === "number" && Number.isFinite(value) && NUMBER_KEYS.has(key)
        ) {
          out[key] = value;
        } else if (typeof value === "boolean" && BOOLEAN_KEYS.has(key)) {
          out[key] = value;
        }
      }
    }
    sink(JSON.stringify(out));
  };
  return {
    info: (event, fields) => write("info", event, fields),
    warn: (event, fields) => write("warn", event, fields),
    error: (event, fields) => write("error", event, fields),
  };
}

export const silentLogger: Logger = {
  info: () => {},
  warn: () => {},
  error: () => {},
};
