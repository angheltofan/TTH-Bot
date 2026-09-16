// TTH gateway entry point.
//
//   deno task start          (reads settings from the environment)
//
// Cloud Run: PORT is provided; TLS is terminated by Cloud Run; set
// TRUST_PROXY=1 and the request timeout to 3600 s. Secrets come from Secret
// Manager as environment variables. LAN development: set TLS_CERT_FILE and
// TLS_KEY_FILE (a dev CA the device pins; the key never enters Git).

import { fakeGeminiConnector } from "./src/fake_gemini.ts";
import { webSocketConnector } from "./src/gemini.ts";
import { createLogger } from "./src/log.ts";
import { createHandler, loadConfig } from "./src/server.ts";

const config = loadConfig((name) => Deno.env.get(name));
const log = createLogger();
const fake = config.geminiMode === "fake";
if (fake) {
  // LAN development only (loadConfig refuses it on Cloud Run or behind a proxy).
  log.warn("gemini_mode", { mode: "fake", code: config.fakeSetup ?? "ok" });
  // Step 6.3: echo of the child's audio at 24 kHz, and the credit test controls.
  log.warn("fake_echo", { ms: config.fakeEcho?.responseDelayMs, count: config.fakeEcho?.repeat });
  if (config.fakeEcho?.firstResponseDelayMs !== undefined) {
    log.warn("fake_first_response", { ms: config.fakeEcho.firstResponseDelayMs });
  }
  log.warn("fake_credit_controls", {
    ms: config.fakeCreditHoldMs,
    count: config.fakeOverCreditFrames,
  });
}
const handler = createHandler({
  config,
  log,
  connectGemini: fake
    ? fakeGeminiConnector({ setup: config.fakeSetup ?? "ok", echo: config.fakeEcho })
    : webSocketConnector(),
  fetchFn: fetch,
  now: () => Date.now(),
});

const tls = config.tlsCertFile && config.tlsKeyFile
  ? {
    cert: await Deno.readTextFile(config.tlsCertFile),
    key: await Deno.readTextFile(config.tlsKeyFile),
  }
  : {};

Deno.serve({ port: config.port, ...tls, onListen: () => log.info("listening", { count: config.port }) }, handler);
