// The Gemini Live side of the gateway.
//
// Everything the M5 must never deal with lives here: Gemini's JSON protocol,
// base64 audio, the model id and voice. The setup message is IDENTICAL to the
// Flutter app's push-to-talk setup (lib/features/voice/gemini_live_service.dart):
// same model, same Puck voice, same generationConfig, and manual activity
// detection — tests/gemini_test.ts pins it field for field.

export const GEMINI_MODEL = "models/gemini-3.1-flash-live-preview";
export const GEMINI_VOICE = "Puck";
// Ephemeral tokens work ONLY with the Constrained method
// (https://ai.google.dev/api/live): the gateway always connects here.
export const GEMINI_WS_URL = "wss://generativelanguage.googleapis.com/ws/" +
  "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContentConstrained";
// Developer API keys use the unconstrained method. Local manual probes only.
export const GEMINI_WS_URL_API_KEY = "wss://generativelanguage.googleapis.com/ws/" +
  "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent";

export function buildSetupMessage(systemInstruction: string): string {
  return JSON.stringify({
    setup: {
      model: GEMINI_MODEL,
      generationConfig: {
        responseModalities: ["AUDIO"],
        speechConfig: {
          voiceConfig: { prebuiltVoiceConfig: { voiceName: GEMINI_VOICE } },
        },
      },
      systemInstruction: { parts: [{ text: systemInstruction }] },
      // Push-to-talk: the device's turn_start/turn_end drive activityStart/
      // activityEnd. The Core2 never uses server VAD (strictly half duplex).
      realtimeInputConfig: { automaticActivityDetection: { disabled: true } },
    },
  });
}

export const ACTIVITY_START = JSON.stringify({ realtimeInput: { activityStart: {} } });
export const ACTIVITY_END = JSON.stringify({ realtimeInput: { activityEnd: {} } });

export function base64Encode(bytes: Uint8Array): string {
  let binary = "";
  const chunk = 0x8000;
  for (let i = 0; i < bytes.length; i += chunk) {
    binary += String.fromCharCode(...bytes.subarray(i, i + chunk));
  }
  return btoa(binary);
}

export function base64Decode(text: string): Uint8Array {
  const binary = atob(text);
  const out = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) out[i] = binary.charCodeAt(i);
  return out;
}

export function audioMessage(pcm16k: Uint8Array): string {
  return JSON.stringify({
    realtimeInput: {
      audio: { mimeType: "audio/pcm;rate=16000", data: base64Encode(pcm16k) },
    },
  });
}

export type GeminiEvent =
  | { type: "setup_complete" }
  | { type: "audio"; pcm: Uint8Array }
  | { type: "interrupted" }
  | { type: "turn_complete" }
  | { type: "go_away" }
  | { type: "error"; message: string };

// Port of the Flutter parseGeminiServerMessage, plus goAway and a format
// check: audio that is not 24 kHz PCM is reported instead of passed on.
export function parseServerMessage(json: unknown): GeminiEvent[] {
  if (json === null || typeof json !== "object") return [];
  const msg = json as Record<string, unknown>;
  if ("setupComplete" in msg) return [{ type: "setup_complete" }];
  if ("goAway" in msg) return [{ type: "go_away" }];

  const events: GeminiEvent[] = [];
  const serverContent = msg.serverContent;
  if (serverContent !== null && typeof serverContent === "object") {
    const content = serverContent as Record<string, unknown>;
    const modelTurn = content.modelTurn as Record<string, unknown> | undefined;
    const parts = modelTurn?.parts;
    if (Array.isArray(parts)) {
      for (const part of parts) {
        const inline = (part as Record<string, unknown> | null)?.inlineData as
          | Record<string, unknown>
          | undefined;
        if (!inline) continue;
        const mime = inline.mimeType;
        const data = inline.data;
        if (typeof mime !== "string" || typeof data !== "string") continue;
        if (!mime.startsWith("audio/")) continue;
        const rate = /rate=(\d+)/.exec(mime)?.[1];
        if (!mime.startsWith("audio/pcm") || (rate !== undefined && rate !== "24000")) {
          events.push({ type: "error", message: "unexpected_audio_format" });
          continue;
        }
        events.push({ type: "audio", pcm: base64Decode(data) });
      }
    }
    if (content.interrupted === true) events.push({ type: "interrupted" });
    if (content.turnComplete === true) events.push({ type: "turn_complete" });
    return events;
  }

  if (msg.error !== undefined) events.push({ type: "error", message: "server_error" });
  return events;
}

export interface GeminiLink {
  send(text: string): void;
  close(): void;
}

export interface GeminiHandlers {
  onEvent(event: GeminiEvent): void;
  onClose(code: number): void;
}

// Opens a Live session with an ephemeral token and sends the setup message.
export type GeminiConnector = (
  token: string,
  setup: string,
  handlers: GeminiHandlers,
) => Promise<GeminiLink>;

// `authParam` is "access_token" for ephemeral tokens (always, in the gateway),
// which connect to the Constrained method. "key" exists only for the local
// manual probe with a developer key, on the unconstrained method.
export function liveSocketUrl(
  token: string,
  authParam: "access_token" | "key" = "access_token",
  url?: string,
): string {
  const base = url ?? (authParam === "key" ? GEMINI_WS_URL_API_KEY : GEMINI_WS_URL);
  return `${base}?${authParam}=${encodeURIComponent(token)}`;
}

export function webSocketConnector(
  url?: string,
  authParam: "access_token" | "key" = "access_token",
): GeminiConnector {
  return (token, setup, handlers) =>
    new Promise((resolve, reject) => {
      let ws: WebSocket;
      try {
        ws = new WebSocket(liveSocketUrl(token, authParam, url));
      } catch {
        // The runtime's message would contain the URL, and so the token.
        reject(new Error("gemini_connect_failed"));
        return;
      }
      ws.binaryType = "arraybuffer";
      let opened = false;
      const decoder = new TextDecoder();
      ws.onopen = () => {
        opened = true;
        ws.send(setup);
        resolve({ send: (text) => ws.send(text), close: () => ws.close() });
      };
      ws.onmessage = (event) => {
        const text = typeof event.data === "string"
          ? event.data
          : decoder.decode(new Uint8Array(event.data as ArrayBuffer));
        let parsed: unknown;
        try {
          parsed = JSON.parse(text);
        } catch {
          handlers.onEvent({ type: "error", message: "undecodable_frame" });
          return;
        }
        for (const e of parseServerMessage(parsed)) handlers.onEvent(e);
      };
      ws.onerror = () => {
        if (!opened) reject(new Error("gemini_connect_failed"));
      };
      ws.onclose = (event) => {
        if (!opened) reject(new Error("gemini_connect_failed"));
        handlers.onClose(event.code);
      };
    });
}
