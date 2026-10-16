/** An unbounded async queue with cancellable waits, used for the inbox and agent responses. */

interface Waiter<T> {
  resolve(r: IteratorResult<T, undefined>): void;
  reject(e: unknown): void;
  timer?: ReturnType<typeof setTimeout>;
}

export class AsyncQueue<T> {
  private readonly items: T[] = [];
  private readonly waiters: Waiter<T>[] = [];
  private ended: { error?: unknown } | null = null;

  get closed(): boolean {
    return this.ended !== null;
  }

  get size(): number {
    return this.items.length;
  }

  push(item: T): void {
    if (this.ended) return;
    const w = this.waiters.shift();
    if (w) {
      if (w.timer !== undefined) clearTimeout(w.timer);
      w.resolve({ done: false, value: item });
    } else {
      this.items.push(item);
    }
  }

  /** Ends the queue: waiters finish (or fail with `error`) once queued items are drained. */
  end(error?: unknown): void {
    if (this.ended) return;
    this.ended = error === undefined ? {} : { error };
    for (const w of this.waiters.splice(0)) this.settleEnded(w);
  }

  /**
   * Takes the next item. With `timeoutMs`, resolves `{ done: true }` when
   * nothing arrives in time, without consuming a later item. `owner` collects
   * the pending waiter so an iterator can cancel it.
   */
  take(timeoutMs?: number, owner?: Set<Waiter<T>>): Promise<IteratorResult<T, undefined>> {
    if (this.items.length > 0) return Promise.resolve({ done: false, value: this.items.shift() as T });
    return new Promise((resolve, reject) => {
      const w: Waiter<T> = {
        resolve: (r) => {
          owner?.delete(w);
          resolve(r);
        },
        reject: (e) => {
          owner?.delete(w);
          reject(e);
        },
      };
      if (this.ended) {
        this.settleEnded(w);
        return;
      }
      if (timeoutMs !== undefined) {
        w.timer = setTimeout(() => {
          this.cancel(w);
        }, timeoutMs);
      }
      owner?.add(w);
      this.waiters.push(w);
    });
  }

  /** Stops a pending take(): it resolves `{ done: true }`. */
  cancel(w: Waiter<T>): void {
    const i = this.waiters.indexOf(w);
    if (i >= 0) this.waiters.splice(i, 1);
    if (w.timer !== undefined) clearTimeout(w.timer);
    w.resolve({ done: true, value: undefined });
  }

  /** An async iterator over the queue; return() (for example `break`) cancels its pending wait. */
  iterator(): AsyncIterableIterator<T> {
    const mine = new Set<Waiter<T>>();
    const it: AsyncIterableIterator<T> = {
      next: () => this.take(undefined, mine),
      return: () => {
        for (const w of [...mine]) this.cancel(w);
        return Promise.resolve({ done: true, value: undefined });
      },
      [Symbol.asyncIterator]() {
        return it;
      },
    };
    return it;
  }

  private settleEnded(w: Waiter<T>): void {
    if (w.timer !== undefined) clearTimeout(w.timer);
    const e = this.ended;
    if (e && "error" in e) w.reject(e.error);
    else w.resolve({ done: true, value: undefined });
  }
}
