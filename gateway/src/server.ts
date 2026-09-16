// HTTP entry: authenticate, rate-limit and validate BEFORE upgrading to a
// WebSocket, then hand the socket to a DeviceSession (PHASE6_PLAN §4.5, §5).
//
//   GET /healthz        liveness
//   GET /v1/ws          device WebSocket, subprotocol tth.v1
//        headers        Authorization: Bearer <device token>
//                       X-TTH-Device: <device id>

import { Activity, fetchEnabledActivities, resolveActivity } from "./activities.ts";
import {
  DEV_ACTIVITY,
  FAKE_ECHO_DEFAULTS,
  FAKE_ECHO_MAX_REPEAT,
  FAKE_RESPONSE_DELAY_MAX_MS,
  FakeEchoOptions,
  FakeSetup,
  parseBoundedInt,
  parseFakeSetup,
} from "./fake_gemini.ts";
import { GeminiConnector } from "./gemini.ts";
import { checkActivity, LIMITS } from "./limits.ts";
import { Logger } from "./log.ts";
import { composeSystemInstruction } from "./prompt.ts";
import { SUBPROTOCOL } from "./protocol.ts";
import { FailureBlocker, TokenBucket } from "./rate_limit.ts";
import { authenticate, parseRegistry, Registry } from "./registry.ts";
import { DeviceSession } from "./session.ts";
import { mintGeminiToken } from "./token_client.ts";

export interface GatewayConfig {
  supabaseUrl: string;
  publishableKey: string;
  mintFunctionUrl: string;
  mintSecret: string;
  registry: Registry;
  port: number;
  trustProxy: boolean;
  tlsCertFile?: string;
  tlsKeyFile?: string;
  // "fake" = LAN development only (src/fake_gemini.ts). Absent = "live".
  geminiMode?: "live" | "fake";
  fakeSetup?: FakeSetup;
  // Step 6.3, fake mode only: the echo response and the credit test controls.
  fakeEcho?: FakeEchoOptions;
  fakeCreditHoldMs?: number;
  fakeOverCreditFrames?: number;
}

// Every setting that only means something with GEMINI_MODE=fake.
export const FAKE_SETTINGS = [
  "FAKE_GEMINI_SETUP",
  "FAKE_RESPONSE_DELAY_MS",
  "FAKE_FIRST_RESPONSE_DELAY_MS",
  "FAKE_ECHO_REPEAT",
  "FAKE_CREDIT_HOLD_MS",
  "FAKE_VIOLATE_CREDIT",
];

export function loadConfig(env: (name: string) => string | undefined): GatewayConfig {
  const need = (name: string): string => {
    const value = env(name);
    if (!value) throw new Error(`missing required setting ${name}`);
    return value;
  };
  const mode = env("GEMINI_MODE") || "live";
  if (mode !== "live" && mode !== "fake") throw new Error("GEMINI_MODE must be live or fake");
  if (mode === "fake") {
    // Never deployable: Cloud Run sets K_SERVICE, and a proxy means it is not
    // a LAN development machine.
    if (env("K_SERVICE") || env("TRUST_PROXY") === "1") {
      throw new Error("GEMINI_MODE=fake is for LAN development only");
    }
    return {
      supabaseUrl: (env("SUPABASE_URL") ?? "").replace(/\/+$/, ""),
      publishableKey: env("SUPABASE_PUBLISHABLE_KEY") ?? "",
      mintFunctionUrl: "",
      mintSecret: "",
      registry: parseRegistry(need("TTH_DEVICES")),
      port: Number(env("PORT") ?? "8080"),
      trustProxy: false,
      tlsCertFile: env("TLS_CERT_FILE"),
      tlsKeyFile: env("TLS_KEY_FILE"),
      geminiMode: "fake",
      fakeSetup: parseFakeSetup(env("FAKE_GEMINI_SETUP")),
      fakeEcho: {
        responseDelayMs: parseBoundedInt(
          env("FAKE_RESPONSE_DELAY_MS"),
          "FAKE_RESPONSE_DELAY_MS",
          0,
          FAKE_RESPONSE_DELAY_MAX_MS,
          FAKE_ECHO_DEFAULTS.responseDelayMs,
        ),
        repeat: parseBoundedInt(
          env("FAKE_ECHO_REPEAT"),
          "FAKE_ECHO_REPEAT",
          1,
          FAKE_ECHO_MAX_REPEAT,
          FAKE_ECHO_DEFAULTS.repeat,
        ),
        ...(env("FAKE_FIRST_RESPONSE_DELAY_MS")
          ? {
            firstResponseDelayMs: parseBoundedInt(
              env("FAKE_FIRST_RESPONSE_DELAY_MS"),
              "FAKE_FIRST_RESPONSE_DELAY_MS",
              0,
              FAKE_RESPONSE_DELAY_MAX_MS,
              0,
            ),
          }
          : {}),
      },
      fakeCreditHoldMs: parseBoundedInt(env("FAKE_CREDIT_HOLD_MS"), "FAKE_CREDIT_HOLD_MS", 0, 60_000, 0),
      fakeOverCreditFrames: parseBoundedInt(env("FAKE_VIOLATE_CREDIT"), "FAKE_VIOLATE_CREDIT", 0, 1, 0),
    };
  }
  for (const name of FAKE_SETTINGS) {
    if (env(name)) throw new Error(`${name} requires GEMINI_MODE=fake`);
  }
  const supabaseUrl = need("SUPABASE_URL").replace(/\/+$/, "");
  const mintSecret = need("GATEWAY_MINT_SECRET");
  if (mintSecret.length < 32) throw new Error("GATEWAY_MINT_SECRET is too short");
  return {
    supabaseUrl,
    publishableKey: need("SUPABASE_PUBLISHABLE_KEY"),
    mintFunctionUrl: env("GATEWAY_MINT_URL") ??
      `${supabaseUrl}/functions/v1/gateway-gemini-token`,
    mintSecret,
    registry: parseRegistry(need("TTH_DEVICES")),
    port: Number(env("PORT") ?? "8080"),
    trustProxy: env("TRUST_PROXY") === "1",
    tlsCertFile: env("TLS_CERT_FILE"),
    tlsKeyFile: env("TLS_KEY_FILE"),
    geminiMode: "live",
  };
}

export interface ServerDeps {
  config: GatewayConfig;
  log: Logger;
  connectGemini: GeminiConnector;
  fetchFn: typeof fetch;
  now: () => number;
  upgrade?: typeof Deno.upgradeWebSocket;
}

const ACTIVITY_CACHE_MS = 30_000;

export function createHandler(deps: ServerDeps) {
  const { config, log } = deps;
  const upgrade = deps.upgrade ?? Deno.upgradeWebSocket;
  const ipBucket = new TokenBucket(LIMITS.connectsPerIpPerMinute);
  const deviceBucket = new TokenBucket(LIMITS.connectsPerDevicePerMinute);
  const turnBuckets = new TokenBucket(LIMITS.turnsPerDevicePerMinute);
  const authFailures = new FailureBlocker(
    LIMITS.authFailuresBeforeBlock,
    LIMITS.authFailureWindowMs,
    LIMITS.authBlockMs,
  );
  const sessions = new Map<string, DeviceSession>();
  // FAKE_VIOLATE_CREDIT: one over-credit frame for the whole process, so the
  // reconnected session that follows is clean.
  const overCreditFrames = { remaining: config.fakeOverCreditFrames ?? 0 };
  const testControls = config.geminiMode === "fake"
    ? { creditHoldMs: config.fakeCreditHoldMs ?? 0, overCreditFrames }
    : undefined;
  let activityCache: { at: number; list: Activity[] } | null = null;

  const activities = async (): Promise<Activity[]> => {
    if (config.geminiMode === "fake" && config.supabaseUrl === "") return [DEV_ACTIVITY];
    const now = deps.now();
    if (activityCache && now - activityCache.at < ACTIVITY_CACHE_MS) return activityCache.list;
    const list = await fetchEnabledActivities(deps.fetchFn, config.supabaseUrl, config.publishableKey);
    activityCache = { at: now, list };
    return list;
  };

  const plain = (status: number, body: string) =>
    new Response(body, { status, headers: { "content-type": "text/plain" } });

  return async (req: Request, info: Deno.ServeHandlerInfo): Promise<Response> => {
    const url = new URL(req.url);
    if (url.pathname === "/healthz") return plain(200, "ok");
    if (url.pathname !== "/v1/ws") return plain(404, "not found");
    if (req.headers.get("upgrade")?.toLowerCase() !== "websocket") {
      return plain(426, "websocket required");
    }
    const protocols = (req.headers.get("sec-websocket-protocol") ?? "")
      .split(",").map((p) => p.trim());
    if (!protocols.includes(SUBPROTOCOL)) return plain(400, "subprotocol required");

    const now = deps.now();
    const forwarded = config.trustProxy
      ? req.headers.get("x-forwarded-for")?.split(",")[0]?.trim()
      : undefined;
    const ip = forwarded ||
      ((info.remoteAddr as Deno.NetAddr | undefined)?.hostname ?? "unknown");

    if (authFailures.isBlocked(ip, now) || !ipBucket.take(ip, now)) {
      log.warn("connect_throttled", { code: "ip" });
      return plain(429, "too many requests");
    }

    const bearer = req.headers.get("authorization");
    const token = bearer?.startsWith("Bearer ") ? bearer.slice(7) : null;
    const deviceId = req.headers.get("x-tth-device");
    const auth = await authenticate(config.registry, deviceId, token);
    if (!auth.ok) {
      authFailures.recordFailure(ip, now);
      log.warn("auth_denied", { reason: auth.reason });
      return plain(401, "unauthorized");
    }
    authFailures.recordSuccess(ip);
    const device = auth.device;
    if (!deviceBucket.take(device.id, now)) {
      log.warn("connect_throttled", { device: device.id, code: "device" });
      return plain(429, "too many requests");
    }

    let resolved;
    try {
      resolved = resolveActivity(await activities(), device.activityId);
    } catch {
      log.error("activities_unavailable", { device: device.id });
      return plain(503, "activities unavailable");
    }
    if (resolved === null) {
      log.warn("activity_unavailable", { device: device.id });
      return plain(503, "activity unavailable");
    }
    const problems = checkActivity(resolved.activity);
    const systemInstruction = composeSystemInstruction(resolved.activity);
    if (problems.length > 0 || systemInstruction.length > LIMITS.maxSystemInstructionChars) {
      log.warn("activity_invalid", {
        device: device.id,
        activity: resolved.activity.id,
        code: problems[0] ?? "system_instruction_too_long",
      });
      return plain(503, "activity invalid");
    }

    const { socket, response } = upgrade(req, { protocol: SUBPROTOCOL });
    socket.binaryType = "arraybuffer";
    const sessionId = crypto.randomUUID();
    const session = new DeviceSession({
      deviceId: device.id,
      sessionId,
      resolved,
      systemInstruction,
      device: {
        sendText: (text) => socket.send(text),
        sendBinary: (data) => socket.send(data),
        bufferedAmount: () => socket.bufferedAmount,
        close: (code, reason) => socket.close(code, reason),
      },
      mintToken: config.geminiMode === "fake"
        ? () => Promise.resolve("fake-gemini-token")
        : () =>
          mintGeminiToken({
            fetchFn: deps.fetchFn,
            functionUrl: config.mintFunctionUrl,
            publishableKey: config.publishableKey,
            secret: config.mintSecret,
            nowMs: deps.now(),
          }),
      connectGemini: deps.connectGemini,
      timers: {
        now: deps.now,
        // Deno's timer id is a number at runtime; the cast only bridges the
        // type declarations.
        set: (fn, ms) => setTimeout(fn, ms) as unknown as number,
        clear: (id) => clearTimeout(id),
      },
      log,
      allowTurn: (t) => turnBuckets.take(device.id, t),
      testControls,
    });

    socket.onopen = () => {
      sessions.get(device.id)?.replace();
      sessions.set(device.id, session);
      log.info("connected", { device: device.id, session: sessionId });
    };
    socket.onmessage = (event) => {
      if (typeof event.data === "string") session.onText(event.data);
      else session.onBinary(new Uint8Array(event.data as ArrayBuffer));
    };
    socket.onclose = () => {
      session.onClose();
      if (sessions.get(device.id) === session) sessions.delete(device.id);
    };
    socket.onerror = () => session.onClose();
    return response;
  };
}
