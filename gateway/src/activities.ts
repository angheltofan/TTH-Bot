// Activity loading and per-device selection (PHASE6_PLAN §5.2, D3, D9).
//
// Reads the `activities` table with the Supabase PUBLISHABLE key (anon read,
// as the Flutter app does). Selection is server-side per device:
//   1. the registry's activity_id for the device, if set — and if that
//      activity is missing or disabled, the session is REFUSED rather than
//      silently given another activity;
//   2. otherwise the first enabled push_to_talk activity (sort_order, title);
//   3. otherwise the first enabled activity of any mode.
// A free_conversation activity runs as push-to-talk on the Core2 (strictly
// half duplex) and the conversion is reported so it can be logged.

export type InteractionMode = "push_to_talk" | "free_conversation";

export interface Activity {
  id: string;
  title: string;
  type: string;
  prompt: string;
  participants: string[];
  interactionMode: InteractionMode;
  enabled: boolean;
  sortOrder: number;
}

export interface ResolvedActivity {
  activity: Activity;
  convertedFromFreeConversation: boolean;
}

// Supabase returns uuid columns in this canonical lowercase form.
export const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
// Bounds on what is read from Supabase: one activity (prompt <= 8 000 chars
// plus fields) and the enabled list.
export const MAX_ACTIVITY_RESPONSE_CHARS = 64_000;
export const MAX_ACTIVITY_LIST_RESPONSE_CHARS = 1_000_000;
const SELECT = "id,title,type,prompt,participants,interaction_mode,enabled,sort_order";

function fail(what: string): never {
  // Field names only: never a value, so a prompt or name cannot end up in an
  // error message.
  throw new Error(`invalid activity row: ${what}`);
}

export function parseActivityRow(row: unknown): Activity {
  if (row === null || typeof row !== "object") fail("not an object");
  const r = row as Record<string, unknown>;
  if (typeof r.id !== "string" || !UUID_RE.test(r.id)) fail("id");
  if (typeof r.title !== "string") fail("title");
  if (typeof r.type !== "string") fail("type");
  if (typeof r.prompt !== "string") fail("prompt");
  const participants = r.participants ?? [];
  if (!Array.isArray(participants) || participants.some((p) => typeof p !== "string")) {
    fail("participants");
  }
  if (r.interaction_mode !== "push_to_talk" && r.interaction_mode !== "free_conversation") {
    fail("interaction_mode");
  }
  // An immutable snapshot: the session built from it cannot drift.
  return Object.freeze({
    id: r.id,
    title: r.title,
    type: r.type,
    prompt: r.prompt,
    participants: Object.freeze([...(participants as string[])]) as string[],
    interactionMode: r.interaction_mode,
    enabled: r.enabled !== false,
    sortOrder: typeof r.sort_order === "number" ? r.sort_order : 0,
  });
}

// Reads rows with the PUBLISHABLE key only (the anon read policy); a
// service_role key is never needed or used here.
async function readRows(
  fetchFn: typeof fetch,
  url: string,
  publishableKey: string,
  maxChars: number,
): Promise<unknown[]> {
  const response = await fetchFn(url, {
    headers: { apikey: publishableKey, Authorization: `Bearer ${publishableKey}` },
  });
  if (!response.ok) {
    await response.body?.cancel();
    throw new Error(`activities request failed: ${response.status}`);
  }
  const text = await response.text();
  if (text.length > maxChars) throw new Error("activities response too large");
  const rows: unknown = JSON.parse(text);
  if (!Array.isArray(rows)) throw new Error("activities response is not an array");
  return rows;
}

export async function fetchEnabledActivities(
  fetchFn: typeof fetch,
  supabaseUrl: string,
  publishableKey: string,
): Promise<Activity[]> {
  const url = `${supabaseUrl}/rest/v1/activities?select=${SELECT}` +
    "&enabled=eq.true&order=sort_order.asc,title.asc";
  const rows = await readRows(fetchFn, url, publishableKey, MAX_ACTIVITY_LIST_RESPONSE_CHARS);
  return rows.map(parseActivityRow);
}

// One activity by id, fresh from Supabase (no cache: every connection gets a
// new snapshot). null when no such row exists. Throws on a malformed id
// (before any request), an HTTP or network failure, an oversized or malformed
// response.
export async function fetchActivityById(
  fetchFn: typeof fetch,
  supabaseUrl: string,
  publishableKey: string,
  id: string,
): Promise<Activity | null> {
  if (!UUID_RE.test(id)) throw new Error("invalid activity id");
  const url = `${supabaseUrl}/rest/v1/activities?select=${SELECT}&id=eq.${id}&limit=2`;
  const rows = await readRows(fetchFn, url, publishableKey, MAX_ACTIVITY_RESPONSE_CHARS);
  if (rows.length === 0) return null;
  if (rows.length > 1) throw new Error("activities response has more than one row");
  const activity = parseActivityRow(rows[0]);
  if (activity.id !== id) throw new Error("activities response has another id");
  return activity;
}

function byOrder(a: Activity, b: Activity): number {
  if (a.sortOrder !== b.sortOrder) return a.sortOrder - b.sortOrder;
  return a.title < b.title ? -1 : a.title > b.title ? 1 : 0;
}

export function resolveActivity(
  activities: readonly Activity[],
  overrideId: string | null,
): ResolvedActivity | null {
  const enabled = activities.filter((a) => a.enabled).slice().sort(byOrder);
  let chosen: Activity | undefined;
  if (overrideId !== null) {
    chosen = enabled.find((a) => a.id === overrideId);
    if (!chosen) return null;
  } else {
    chosen = enabled.find((a) => a.interactionMode === "push_to_talk") ?? enabled[0];
    if (!chosen) return null;
  }
  return {
    activity: chosen,
    convertedFromFreeConversation: chosen.interactionMode === "free_conversation",
  };
}
