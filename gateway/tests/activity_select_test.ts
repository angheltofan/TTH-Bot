// On-device activity selection: the protocol messages (byte-exact vectors
// shared with firmware/core2/test/test_gateway_protocol and
// test_activity_selector), the bounded selectable list, restoring a saved
// selection at hello, switching activity mid-connection, and every failure
// path keeping the previous activity usable.

import { assert, assertEquals, assertThrows } from "./assert.ts";
import { Activity, ActivitySummary, selectableActivities } from "../src/activities.ts";
import { ACTIVITY_START } from "../src/gemini.ts";
import { createLogger } from "../src/log.ts";
import { composeSystemInstruction } from "../src/prompt.ts";
import {
  encodeActivityList,
  encodeActivitySelected,
  encodeActivitySelectError,
  parseDeviceMessage,
  safeActivityTitle,
  utf8Length,
} from "../src/protocol.ts";
import { ActivityLoad, DeviceSession } from "../src/session.ts";
import { FakeDevice, FakeGemini, FakeLink, FakeTimers, flush } from "./session_harness.ts";

const A = "00000000-0000-4000-8000-00000000000a";
const B = "00000000-0000-4000-8000-00000000000b";
const MISSING = "00000000-0000-4000-8000-00000000000c";
const VECTOR_ID = "a95ffc7e-1406-4a19-ac3b-6c27d8516b70";

function activity(id: string, overrides: Partial<Activity> = {}): Activity {
  return {
    id,
    title: `TITLESENTINEL ${id.slice(-1)}`,
    type: "lesson",
    prompt: `PROMPTSENTINEL for ${id.slice(-1)}`,
    participants: ["Sentinelia"],
    interactionMode: "push_to_talk",
    enabled: true,
    sortOrder: 0,
    ...overrides,
  };
}

function ok(a: Activity): ActivityLoad {
  return {
    ok: true,
    resolved: { activity: a, convertedFromFreeConversation: false },
    systemInstruction: composeSystemInstruction(a),
  };
}

const SENTINELS = ["PROMPTSENTINEL", "Sentinelia"];

// --- protocol ----------------------------------------------------------------------------

Deno.test("activity messages: the same byte-exact vectors as the firmware", () => {
  assertEquals(
    encodeActivityList(1, 4, VECTOR_ID, "Conversație liberă", "free_conversation", true),
    `{"t":"activity_list","index":1,"count":4,"activity":"${VECTOR_ID}","title":"Conversație liberă","mode":"free_conversation","current":true}`,
  );
  assertEquals(encodeActivitySelected(VECTOR_ID), `{"t":"activity_selected","activity":"${VECTOR_ID}"}`);
  assertEquals(
    encodeActivitySelectError("missing", VECTOR_ID),
    `{"t":"activity_select_error","code":"missing","activity":"${VECTOR_ID}"}`,
  );
  assertEquals(encodeActivitySelectError("busy"), `{"t":"activity_select_error","code":"busy"}`);

  const hello = parseDeviceMessage(
    `{"t":"hello","proto":1,"fw":"core2-6.4","in":"s16le/16000/1","out":"s16le/24000/1","maxDown":1920,"credit":192000,"activity":"${VECTOR_ID}"}`,
  );
  assert(hello.ok && hello.msg.t === "hello" && hello.msg.activity === VECTOR_ID, "hello with activity");
  const plain = parseDeviceMessage(
    `{"t":"hello","proto":1,"fw":"core2-6.3","in":"s16le/16000/1","out":"s16le/24000/1","maxDown":1920,"credit":192000}`,
  );
  assert(plain.ok && plain.msg.t === "hello" && plain.msg.activity === undefined, "older hello");
  const select = parseDeviceMessage(`{"t":"activity_select","activity":"${VECTOR_ID}"}`);
  assert(select.ok && select.msg.t === "activity_select" && select.msg.activity === VECTOR_ID, "select");
});

Deno.test("activity messages: ids, positions and titles are bounded", () => {
  for (const bad of ["nope", VECTOR_ID.toUpperCase(), `${VECTOR_ID}x`]) {
    const r = parseDeviceMessage(`{"t":"activity_select","activity":"${bad}"}`);
    assert(!r.ok && r.error === "bad_value", `select ${bad}`);
    const h = parseDeviceMessage(
      `{"t":"hello","proto":1,"fw":"x","in":"s16le/16000/1","out":"s16le/24000/1","maxDown":1920,"credit":1,"activity":"${bad}"}`,
    );
    assert(!h.ok && h.error === "bad_value", `hello ${bad}`);
  }
  const missing = parseDeviceMessage(`{"t":"activity_select"}`);
  assert(!missing.ok && missing.error === "missing_field", "missing activity");
  assertThrows(() => encodeActivityList(0, 0, VECTOR_ID, "T", "push_to_talk", false));
  assertThrows(() => encodeActivityList(0, 9, VECTOR_ID, "T", "push_to_talk", false));
  assertThrows(() => encodeActivityList(4, 4, VECTOR_ID, "T", "push_to_talk", false));
  assertThrows(() => encodeActivityList(0, 1, "nope", "T", "push_to_talk", false));
  assertThrows(() => encodeActivityList(0, 1, VECTOR_ID, 'A "quote"', "push_to_talk", false));
  assertThrows(() => encodeActivityList(0, 1, VECTOR_ID, "x".repeat(49), "push_to_talk", false));
  assertThrows(() => encodeActivitySelected("nope"));
});

Deno.test("titles: no quote, backslash or control character, at most 48 UTF-8 bytes, never empty", () => {
  assertEquals(safeActivityTitle('  Ana "are"\\ mere\n\tazi '), "Ana are mere azi");
  const long = "Știință și învățătură ".repeat(4);
  const cut = safeActivityTitle(long);
  assert(utf8Length(cut) <= 48, "bounded");
  assert(long.startsWith(cut), "cut on a character boundary");
  assertEquals(safeActivityTitle(""), "Activitate");
  assertEquals(safeActivityTitle("Conversație liberă"), "Conversație liberă");
});

Deno.test("the selectable list: usable activities only, in order, at most 8, metadata only", () => {
  const list: Activity[] = [
    activity(A, { sortOrder: 2 }),
    activity(B, { sortOrder: 1, title: 'Joc "nou"' }),
    activity("00000000-0000-4000-8000-000000000001", { enabled: false }),
    activity("00000000-0000-4000-8000-000000000002", { prompt: "p".repeat(8_001) }),
    activity("00000000-0000-4000-8000-000000000003", { participants: Array(13).fill("Ana") }),
    ...Array.from({ length: 10 }, (_, i) =>
      activity(`00000000-0000-4000-8000-0000000001${String(i).padStart(2, "0")}`, { sortOrder: 5 + i })),
  ];
  const summaries = selectableActivities(list);
  assertEquals(summaries.length, 8);
  assertEquals(summaries[0].id, B);
  assertEquals(summaries[0].title, "Joc nou");
  assertEquals(summaries[1].id, A);
  for (const s of summaries) {
    assertEquals(Object.keys(s).sort().join(","), "id,mode,title");
    assert(!summaries.some((x) => x.id.endsWith("000000000001")), "no disabled");
  }
  assert(!JSON.stringify(summaries).includes("PROMPTSENTINEL"), "no prompt");
  assert(!JSON.stringify(summaries).includes("Sentinelia"), "no participants");
});

// --- session ----------------------------------------------------------------------------------

interface Options {
  db?: Record<string, ActivityLoad | Error>;
  list?: ActivitySummary[];
  failConnects?: number[]; // 1-based connector calls that fail
}

function build(options: Options = {}) {
  const timers = new FakeTimers();
  const device = new FakeDevice();
  const gemini = new FakeGemini();
  const logs: string[] = [];
  const loads: string[] = [];
  let connects = 0;
  const connector = gemini.connector;
  gemini.connector = (token, setup, handlers) => {
    connects++;
    if (options.failConnects?.includes(connects)) return Promise.reject(new Error("down"));
    return connector(token, setup, handlers);
  };
  const db = options.db ?? { [A]: ok(activity(A)), [B]: ok(activity(B)) };
  const session = new DeviceSession({
    deviceId: "core2-01",
    sessionId: "s-1",
    resolved: { activity: activity(A), convertedFromFreeConversation: false },
    systemInstruction: composeSystemInstruction(activity(A)),
    device,
    mintToken: () => Promise.resolve("tok"),
    connectGemini: (t, s, h) => gemini.connector(t, s, h),
    timers,
    log: createLogger((line) => logs.push(line), () => "T"),
    loadActivity: (id) => {
      loads.push(id);
      const entry = db[id];
      if (entry instanceof Error) return Promise.reject(entry);
      return Promise.resolve(entry ?? { ok: false, code: "missing" });
    },
    listActivities: () =>
      Promise.resolve(options.list ?? [
        { id: A, title: "Activitate A", mode: "push_to_talk" },
        { id: B, title: "Activitate B", mode: "free_conversation" },
      ]),
  });
  return { timers, device, gemini, logs, loads, session };
}

type H = ReturnType<typeof build>;

const settle = async () => {
  for (let i = 0; i < 4; i++) await flush();
};

function hello(activityId?: string): string {
  const extra = activityId === undefined ? "" : `,"activity":"${activityId}"`;
  return `{"t":"hello","proto":1,"fw":"core2-6.4","in":"s16le/16000/1","out":"s16le/24000/1","maxDown":1920,"credit":192000${extra}}`;
}

async function readyUp(h: H, activityId?: string) {
  h.session.onText(hello(activityId));
  await settle();
  h.gemini.last.emit({ type: "setup_complete" });
  await settle();
}

function instruction(link: FakeLink): string {
  return JSON.parse(link.setup).setup.systemInstruction.parts[0].text;
}

// deno-lint-ignore no-explicit-any
function texts(h: H): any[] {
  return h.device.sent.filter((s) => s.kind === "text").map((s) => (s as { msg: unknown }).msg);
}

function takeTypes(h: H): string[] {
  return h.device.take().filter((t) => !t.startsWith("audio"));
}

Deno.test("after ready the device gets the selectable list: id, title, mode, current only", async () => {
  const h = build();
  await readyUp(h);
  const msgs = texts(h);
  assertEquals(msgs.map((m) => m.t), ["ready", "activity_list", "activity_list"]);
  assertEquals(Object.keys(msgs[1]).sort().join(","), "activity,count,current,index,mode,t,title");
  assertEquals([msgs[1].index, msgs[1].count, msgs[1].activity, msgs[1].current], [0, 2, A, true]);
  assertEquals([msgs[2].index, msgs[2].activity, msgs[2].current], [1, B, false]);
  for (const s of SENTINELS) assert(!JSON.stringify(h.device.sent).includes(s), `device never gets ${s}`);
});

Deno.test("a saved activity sent in hello opens the first Gemini session with it", async () => {
  const h = build();
  await readyUp(h, B);
  assertEquals(h.loads, [B]);
  assertEquals(h.gemini.links.length, 1);
  assertEquals(instruction(h.gemini.last), composeSystemInstruction(activity(B)));
  const ready = texts(h).find((m) => m.t === "ready");
  assertEquals(ready.activity, B);
  assertEquals(texts(h).find((m) => m.t === "activity_list" && m.current).activity, B);
});

Deno.test("an unusable saved activity keeps the configured one and tells the device why", async () => {
  for (const [code, entry] of [
    ["missing", undefined],
    ["disabled", { ok: false, code: "disabled" } as ActivityLoad],
    ["invalid", { ok: false, code: "invalid" } as ActivityLoad],
    ["unavailable", new Error("supabase down")],
  ] as const) {
    const h = build({ db: { [A]: ok(activity(A)), ...(entry ? { [MISSING]: entry } : {}) } });
    await readyUp(h, MISSING);
    const error = texts(h).find((m) => m.t === "activity_select_error");
    assertEquals([error.code, error.activity], [code, MISSING]);
    assertEquals(instruction(h.gemini.last), composeSystemInstruction(activity(A)));
    assertEquals(texts(h).find((m) => m.t === "ready").activity, A);
  }
});

Deno.test("a malformed saved activity is a protocol error", async () => {
  const h = build();
  h.session.onText(hello("NOT-A-UUID"));
  await settle();
  assertEquals(h.device.closed?.code, 1002);
  assertEquals(h.gemini.links.length, 0);
});

Deno.test("selecting an activity loads it, replaces the Gemini session, then confirms", async () => {
  const h = build();
  await readyUp(h);
  takeTypes(h);
  const oldLink = h.gemini.last;

  h.session.onText(`{"t":"activity_select","activity":"${B}"}`);
  await settle();
  assertEquals(h.loads, [B]);
  assert(oldLink.closed, "the old Gemini session is closed");
  assertEquals(h.gemini.links.length, 2);
  assertEquals(instruction(h.gemini.last), composeSystemInstruction(activity(B)));
  // Not confirmed until the new session is up; turns are refused meanwhile.
  assertEquals(takeTypes(h), []);
  h.session.onText(`{"t":"turn_start","turn":1}`);
  await settle();
  const busy = texts(h).find((m) => m.t === "error");
  assertEquals([busy.code, busy.turn], ["busy", 1]);
  h.device.take();

  h.gemini.last.emit({ type: "setup_complete" });
  await settle();
  const selected = texts(h);
  assertEquals(selected.map((m) => m.t), ["activity_selected"]);
  assertEquals(selected[0].activity, B);

  // The new session is usable.
  h.session.onText(`{"t":"turn_start","turn":2}`);
  await settle();
  assert(h.gemini.last.sent.includes(ACTIVITY_START), "turn reaches the new session");
});

Deno.test("a refused selection keeps the previous activity and its Gemini session", async () => {
  for (const [code, entry] of [
    ["missing", undefined],
    ["disabled", { ok: false, code: "disabled" } as ActivityLoad],
    ["invalid", { ok: false, code: "invalid" } as ActivityLoad],
    ["unavailable", new Error("network")],
  ] as const) {
    const h = build({ db: { [A]: ok(activity(A)), ...(entry ? { [B]: entry } : {}) } });
    await readyUp(h);
    h.device.take();
    const link = h.gemini.last;
    h.session.onText(`{"t":"activity_select","activity":"${B}"}`);
    await settle();
    const msgs = texts(h);
    assertEquals(msgs.map((m) => m.t), ["activity_select_error"]);
    assertEquals([msgs[0].code, msgs[0].activity], [code, B]);
    assert(!link.closed, "the Gemini session stays open");
    assertEquals(h.gemini.links.length, 1);
    h.session.onText(`{"t":"turn_start","turn":1}`);
    await settle();
    assert(link.sent.includes(ACTIVITY_START), "still usable");
  }
});

Deno.test("selection is refused while a turn is in progress, and the current one confirms at once", async () => {
  const h = build();
  await readyUp(h);
  h.device.take();
  h.session.onText(`{"t":"turn_start","turn":1}`);
  h.session.onText(`{"t":"activity_select","activity":"${B}"}`);
  await settle();
  assertEquals(texts(h).map((m) => [m.t, m.code]), [["activity_select_error", "busy"]]);
  assertEquals(h.loads, []);

  const idle = build();
  await readyUp(idle);
  idle.device.take();
  idle.session.onText(`{"t":"activity_select","activity":"${A}"}`);
  await settle();
  assertEquals(texts(idle).map((m) => [m.t, m.activity]), [["activity_selected", A]]);
  assertEquals(idle.gemini.links.length, 1);
  assertEquals(idle.loads, []);
});

Deno.test("if the new Gemini session fails, the previous activity is restored", async () => {
  const h = build({ failConnects: [2] });
  await readyUp(h);
  h.device.take();
  h.session.onText(`{"t":"activity_select","activity":"${B}"}`);
  await settle();
  const error = texts(h).find((m) => m.t === "activity_select_error");
  assertEquals([error.code, error.activity], ["gemini_unavailable", B]);
  // The next turn reopens Gemini with the PREVIOUS activity.
  h.session.onText(`{"t":"turn_start","turn":1}`);
  await settle();
  assertEquals(instruction(h.gemini.last), composeSystemInstruction(activity(A)));
});

Deno.test("no prompt, title or participant reaches the logs in any selection flow", async () => {
  const all: string[] = [];
  const flows: Array<(h: H) => Promise<void>> = [
    async (h) => await readyUp(h, B),
    async (h) => await readyUp(h, MISSING),
    async (h) => {
      await readyUp(h);
      h.session.onText(`{"t":"activity_select","activity":"${B}"}`);
      await settle();
      h.gemini.last.emit({ type: "setup_complete" });
      await settle();
    },
    async (h) => {
      await readyUp(h);
      h.session.onText(`{"t":"activity_select","activity":"${MISSING}"}`);
      await settle();
    },
  ];
  for (const flow of flows) {
    const h = build();
    await flow(h);
    all.push(...h.logs);
  }
  assert(all.some((l) => l.includes("activity_selected")), "the selection is logged");
  for (const line of all) {
    for (const s of [...SENTINELS, "TITLESENTINEL", "Activitate A"]) {
      assert(!line.includes(s), `log must not contain ${s}`);
    }
  }
});
