// In-memory rate limiting, per gateway instance (PHASE6_PLAN §4.5).

export class TokenBucket {
  #capacity: number;
  #refillPerMs: number;
  #buckets = new Map<string, { tokens: number; at: number }>();

  // `perMinute` requests, refilled continuously, with a burst of `perMinute`.
  constructor(perMinute: number) {
    this.#capacity = perMinute;
    this.#refillPerMs = perMinute / 60_000;
  }

  take(key: string, nowMs: number): boolean {
    const bucket = this.#buckets.get(key) ?? { tokens: this.#capacity, at: nowMs };
    const elapsed = Math.max(0, nowMs - bucket.at);
    bucket.tokens = Math.min(this.#capacity, bucket.tokens + elapsed * this.#refillPerMs);
    bucket.at = nowMs;
    if (bucket.tokens < 1) {
      this.#buckets.set(key, bucket);
      return false;
    }
    bucket.tokens -= 1;
    this.#buckets.set(key, bucket);
    return true;
  }

  prune(nowMs: number): void {
    for (const [key, bucket] of this.#buckets) {
      if (nowMs - bucket.at > 10 * 60_000) this.#buckets.delete(key);
    }
  }
}

// Blocks a key after `maxFailures` failures within `windowMs`, for `blockMs`.
export class FailureBlocker {
  #max: number;
  #windowMs: number;
  #blockMs: number;
  #state = new Map<string, { failures: number[]; blockedUntil: number }>();

  constructor(maxFailures: number, windowMs: number, blockMs: number) {
    this.#max = maxFailures;
    this.#windowMs = windowMs;
    this.#blockMs = blockMs;
  }

  isBlocked(key: string, nowMs: number): boolean {
    const s = this.#state.get(key);
    return s !== undefined && s.blockedUntil > nowMs;
  }

  recordFailure(key: string, nowMs: number): void {
    const s = this.#state.get(key) ?? { failures: [], blockedUntil: 0 };
    s.failures = s.failures.filter((t) => nowMs - t < this.#windowMs);
    s.failures.push(nowMs);
    if (s.failures.length >= this.#max) {
      s.blockedUntil = nowMs + this.#blockMs;
      s.failures = [];
    }
    this.#state.set(key, s);
  }

  recordSuccess(key: string): void {
    this.#state.delete(key);
  }
}
