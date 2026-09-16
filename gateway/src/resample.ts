// 16 kHz -> 24 kHz mono s16le, for the LAN fake Gemini ONLY (Step 6.3).
//
// The fake echoes the child's captured audio back as "model speech". The
// device plays downstream audio at 24 kHz, so the echo is converted here, in
// the gateway's development-only path. The firmware has no resampler, and the
// Phase 5 rule (every stream plays at its native rate) is unchanged.
//
// Deterministic linear interpolation. Output sample j sits at input position
// j * 2/3, so the output length is exactly floor(n * 3 / 2): an even input
// sample count converts with no duration error at all. The input is bounded by
// the per-turn user-audio limit, and the output is allocated once, at its exact
// size. Samples are never logged.

import { LIMITS } from "./limits.ts";

export const MAX_RESAMPLE_INPUT_BYTES = LIMITS.maxUserAudioBytesPerTurn;

// Output samples for `inputSamples` at 16 kHz.
export function resampledSamples(inputSamples: number): number {
  return Math.floor((inputSamples * 3) / 2);
}

export function resample16kTo24k(input: Uint8Array): Uint8Array {
  if (input.length % 2 !== 0) {
    throw new RangeError("s16le input must have an even byte length");
  }
  if (input.length > MAX_RESAMPLE_INPUT_BYTES) {
    throw new RangeError("input exceeds the maximum user turn");
  }
  const inSamples = input.length / 2;
  const outSamples = resampledSamples(inSamples);
  const out = new Uint8Array(outSamples * 2);
  if (outSamples === 0) return out;

  const src = new DataView(input.buffer, input.byteOffset, input.byteLength);
  const dst = new DataView(out.buffer);
  for (let j = 0; j < outSamples; j++) {
    const position = j * 2; // in thirds of an input sample
    const i = Math.floor(position / 3);
    const frac = position - i * 3; // 0, 1 or 2 thirds
    const a = src.getInt16(i * 2, true);
    const b = i + 1 < inSamples ? src.getInt16((i + 1) * 2, true) : a;
    const value = Math.round((a * (3 - frac) + b * frac) / 3);
    dst.setInt16(j * 2, value > 32767 ? 32767 : value < -32768 ? -32768 : value, true);
  }
  return out;
}
