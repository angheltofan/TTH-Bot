// Gateway-side turn-id validation (PHASE6_PLAN §2.2).
//
// Turn ids are allocated by the device (uint32, 0 reserved). The gateway
// refuses a turn_start that reuses the active id or one of the last 16 ended
// or cancelled ids, so a late frame of an old turn can never be attributed to
// a new one.

export const RECENT_TURNS = 16;

export type StartCheck = "ok" | "zero" | "active" | "recent" | "busy";

export class TurnRegistry {
  #active: number | null = null;
  #recent: number[] = [];

  get active(): number | null {
    return this.#active;
  }

  isRecent(turn: number): boolean {
    return this.#recent.includes(turn);
  }

  validateStart(turn: number): StartCheck {
    if (turn === 0) return "zero";
    if (this.#active === turn) return "active";
    if (this.#active !== null) return "busy";
    if (this.isRecent(turn)) return "recent";
    return "ok";
  }

  start(turn: number): void {
    const check = this.validateStart(turn);
    if (check !== "ok") throw new Error(`turn_start refused: ${check}`);
    this.#active = turn;
  }

  // The upstream half of the turn ended (turn_end) or was cancelled.
  closeUpstream(turn: number): void {
    if (this.#active === turn) this.#active = null;
    this.#remember(turn);
  }

  #remember(turn: number): void {
    if (turn === 0 || this.isRecent(turn)) return;
    this.#recent.push(turn);
    if (this.#recent.length > RECENT_TURNS) this.#recent.shift();
  }
}
