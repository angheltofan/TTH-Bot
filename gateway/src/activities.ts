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

function fail(what: string): never {
  throw new Error(`invalid activity row: ${what}`);
}

export function parseActivityRow(row: unknown): Activity {
  if (row === null || typeof row !== "object") fail("not an object");
  const r = row as Record<string, unknown>;
  if (typeof r.id !== "string") fail("id");
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
  return {
    id: r.id,
    title: r.title,
    type: r.type,
    prompt: r.prompt,
    participants: participants as string[],
    interactionMode: r.interaction_mode,
    enabled: r.enabled !== false,
    sortOrder: typeof r.sort_order === "number" ? r.sort_order : 0,
  };
}

export async function fetchEnabledActivities(
  fetchFn: typeof fetch,
  supabaseUrl: string,
  publishableKey: string,
): Promise<Activity[]> {
  const url = `${supabaseUrl}/rest/v1/activities` +
    "?select=id,title,type,prompt,participants,interaction_mode,enabled,sort_order" +
    "&enabled=eq.true&order=sort_order.asc,title.asc";
  const response = await fetchFn(url, {
    headers: { apikey: publishableKey, Authorization: `Bearer ${publishableKey}` },
  });
  if (!response.ok) throw new Error(`activities request failed: ${response.status}`);
  const rows: unknown = await response.json();
  if (!Array.isArray(rows)) throw new Error("activities response is not an array");
  return rows.map(parseActivityRow);
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
