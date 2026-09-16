// Device registry and authentication (PHASE6_PLAN §3, §4.5, D5).
//
// The pilot registry is a secret (Cloud Run Secret Manager), JSON:
//   { "<device id>": { "token_sha256": "<64 hex>", "activity_id": "<uuid>|null",
//                      "enabled": true } }
// Only SHA-256 digests of device tokens are stored. Comparison is
// constant-time, and an unknown device costs the same work as a known one.

export interface DeviceRecord {
  id: string;
  tokenSha256: Uint8Array;
  activityId: string | null;
  enabled: boolean;
}

export type Registry = ReadonlyMap<string, DeviceRecord>;

export const DEVICE_ID_RE = /^[A-Za-z0-9_-]{1,32}$/;
// 32 random bytes, base64url without padding.
export const DEVICE_TOKEN_RE = /^[A-Za-z0-9_-]{43}$/;
const HEX64_RE = /^[0-9a-f]{64}$/;
const UUID_RE =
  /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;

function hexToBytes(hex: string): Uint8Array {
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) {
    out[i] = parseInt(hex.slice(2 * i, 2 * i + 2), 16);
  }
  return out;
}

export function parseRegistry(json: string): Registry {
  const value: unknown = JSON.parse(json);
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    throw new Error("registry must be a JSON object");
  }
  const out = new Map<string, DeviceRecord>();
  for (const [id, raw] of Object.entries(value as Record<string, unknown>)) {
    if (!DEVICE_ID_RE.test(id)) throw new Error("invalid device id in registry");
    if (raw === null || typeof raw !== "object") {
      throw new Error("invalid registry entry");
    }
    const entry = raw as Record<string, unknown>;
    const digest = entry.token_sha256;
    if (typeof digest !== "string" || !HEX64_RE.test(digest)) {
      throw new Error("invalid token_sha256 in registry");
    }
    const activity = entry.activity_id ?? null;
    if (activity !== null && (typeof activity !== "string" || !UUID_RE.test(activity))) {
      throw new Error("invalid activity_id in registry");
    }
    if (entry.enabled !== undefined && typeof entry.enabled !== "boolean") {
      throw new Error("invalid enabled flag in registry");
    }
    out.set(id, {
      id,
      tokenSha256: hexToBytes(digest),
      activityId: activity as string | null,
      enabled: entry.enabled !== false,
    });
  }
  return out;
}

export async function sha256(bytes: Uint8Array): Promise<Uint8Array> {
  return new Uint8Array(await crypto.subtle.digest("SHA-256", bytes as BufferSource));
}

// Constant time for equal lengths; different lengths are simply unequal (the
// lengths here are fixed 32-byte digests).
export function timingSafeEqual(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
  return diff === 0;
}

export type AuthResult =
  | { ok: true; device: DeviceRecord }
  | { ok: false; reason: "missing" | "malformed" | "unknown" | "disabled" | "bad_token" };

const DUMMY_DIGEST = new Uint8Array(32);
const encoder = new TextEncoder();

export async function authenticate(
  registry: Registry,
  deviceId: string | null,
  token: string | null,
): Promise<AuthResult> {
  if (!deviceId || !token) return { ok: false, reason: "missing" };
  const wellFormed = DEVICE_ID_RE.test(deviceId) && DEVICE_TOKEN_RE.test(token);
  // Always hash and compare, so timing does not reveal which check failed.
  const presented = await sha256(encoder.encode(token));
  const record = wellFormed ? registry.get(deviceId) : undefined;
  const matches = timingSafeEqual(presented, record?.tokenSha256 ?? DUMMY_DIGEST);
  if (!wellFormed) return { ok: false, reason: "malformed" };
  if (!record) return { ok: false, reason: "unknown" };
  if (!matches) return { ok: false, reason: "bad_token" };
  if (!record.enabled) return { ok: false, reason: "disabled" };
  return { ok: true, device: record };
}
