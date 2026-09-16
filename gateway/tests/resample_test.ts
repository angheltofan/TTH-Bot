// The LAN fake Gemini's 16 kHz -> 24 kHz resampler (Step 6.3).

import { assert, assertEquals } from "./assert.ts";
import { LIMITS } from "../src/limits.ts";
import {
  MAX_RESAMPLE_INPUT_BYTES,
  resample16kTo24k,
  resampledSamples,
} from "../src/resample.ts";

function pcm(samples: number[]): Uint8Array {
  const out = new Uint8Array(samples.length * 2);
  const view = new DataView(out.buffer);
  samples.forEach((s, i) => view.setInt16(i * 2, s, true));
  return out;
}

function samplesOf(bytes: Uint8Array): number[] {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const out: number[] = [];
  for (let i = 0; i < bytes.length / 2; i++) out.push(view.getInt16(i * 2, true));
  return out;
}

Deno.test("sample counts and durations convert exactly", () => {
  assertEquals(resampledSamples(0), 0);
  assertEquals(resampledSamples(1), 1);
  assertEquals(resampledSamples(2), 3);
  assertEquals(resampledSamples(320), 480); // one 20 ms up frame -> 20 ms at 24 kHz
  assertEquals(resampledSamples(16000), 24000); // exactly 1 s
  assertEquals(resample16kTo24k(new Uint8Array(640)).length, 960);
  assertEquals(resample16kTo24k(new Uint8Array(32000)).length, 48000);
});

Deno.test("empty and partial final frames", () => {
  assertEquals(resample16kTo24k(new Uint8Array(0)).length, 0);
  // A 101-sample tail (the last, partial frame of a turn).
  assertEquals(resample16kTo24k(new Uint8Array(202)).length, 151 * 2);
  let threw = false;
  try {
    resample16kTo24k(new Uint8Array(3));
  } catch {
    threw = true;
  }
  assert(threw, "an odd byte length must be refused");
});

Deno.test("interpolation is deterministic and keeps sample order", () => {
  const out = samplesOf(resample16kTo24k(pcm([0, 300, 600, 900])));
  // positions 0, 2/3, 4/3, 2, 8/3, 10/3 (the last clamps to the final sample)
  assertEquals(out, [0, 200, 400, 600, 800, 900]);
  // A rising ramp across what were separate frames stays strictly ordered.
  const ramp = Array.from({ length: 640 }, (_, i) => i * 50 - 16000);
  const up = samplesOf(resample16kTo24k(pcm(ramp)));
  for (let i = 1; i < up.length - 1; i++) assert(up[i] > up[i - 1], `order at ${i}`);
});

Deno.test("full-scale input stays within int16", () => {
  const out = samplesOf(resample16kTo24k(pcm([32767, -32768, 32767, -32768, 32767])));
  for (const s of out) assert(s <= 32767 && s >= -32768, "saturated");
  assertEquals(out[0], 32767);
  assertEquals(out[1], -10923); // (32767 + 2 * -32768) / 3, rounded
  assertEquals(out[3], 32767); // lands exactly on input sample 2
});

Deno.test("the maximum turn converts and anything larger is refused", () => {
  assertEquals(MAX_RESAMPLE_INPUT_BYTES, LIMITS.maxUserAudioBytesPerTurn);
  const max = new Uint8Array(MAX_RESAMPLE_INPUT_BYTES);
  const out = resample16kTo24k(max);
  assertEquals(out.length, resampledSamples(MAX_RESAMPLE_INPUT_BYTES / 2) * 2);
  assert(out.length <= LIMITS.maxModelAudioBytesPerTurn, "fits the model audio limit");
  let threw = false;
  try {
    resample16kTo24k(new Uint8Array(MAX_RESAMPLE_INPUT_BYTES + 2));
  } catch {
    threw = true;
  }
  assert(threw, "over the per-turn limit must be refused");
});
