// Downstream credit, gateway side (PHASE6_PLAN §3.3).
//
// The device grants `initial` PCM bytes in `hello` (its playback ring's free
// space). The gateway sends model audio only within the remaining credit and
// regains it only when the device returns it with `credit {bytes}`. A device
// returning more than was ever sent is a protocol error.

import { isU32 } from "./protocol.ts";

export class CreditLedger {
  readonly initial: number;
  #sent = 0;
  #returned = 0;

  constructor(initial: number) {
    if (!isU32(initial)) throw new RangeError("invalid initial credit");
    this.initial = initial;
  }

  get available(): number {
    return this.initial - (this.#sent - this.#returned);
  }

  get outstanding(): number {
    return this.#sent - this.#returned;
  }

  get sent(): number {
    return this.#sent;
  }

  get returned(): number {
    return this.#returned;
  }

  // Spends credit for one frame of `bytes` PCM, or refuses without spending.
  trySpend(bytes: number): boolean {
    if (bytes <= 0 || bytes > this.available) return false;
    this.#sent += bytes;
    return true;
  }

  // LAN FAKE MODE TEST CONTROL ONLY (FAKE_VIOLATE_CREDIT): records a frame
  // sent beyond the device's credit, so the device's refusal can be tested.
  // Never used by the live path.
  forceSpend(bytes: number): void {
    this.#sent += bytes;
  }

  applyReturn(bytes: number): "ok" | "over_return" {
    if (!isU32(bytes)) return "over_return";
    if (this.#returned + bytes > this.#sent) return "over_return";
    this.#returned += bytes;
    return "ok";
  }
}
