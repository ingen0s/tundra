/** A tiny typed event emitter that works the same in browsers and Node. */

export type Listener<T> = (value: T) => void;

export class Emitter<Events extends object> {
  private readonly listeners = new Map<keyof Events, Set<Listener<never>>>();

  /** Adds a listener; returns a function that removes it. */
  on<K extends keyof Events>(event: K, listener: Listener<Events[K]>): () => void {
    let set = this.listeners.get(event);
    if (!set) {
      set = new Set();
      this.listeners.set(event, set);
    }
    set.add(listener as Listener<never>);
    return () => this.off(event, listener);
  }

  off<K extends keyof Events>(event: K, listener: Listener<Events[K]>): void {
    this.listeners.get(event)?.delete(listener as Listener<never>);
  }

  /** Adds a listener that runs once. */
  once<K extends keyof Events>(event: K, listener: Listener<Events[K]>): () => void {
    const off = this.on(event, (value) => {
      off();
      listener(value);
    });
    return off;
  }

  /** Resolves with the next value of `event`. */
  next<K extends keyof Events>(event: K): Promise<Events[K]> {
    return new Promise((resolve) => this.once(event, resolve));
  }

  protected emit<K extends keyof Events>(event: K, value: Events[K]): void {
    const set = this.listeners.get(event);
    if (!set) return;
    for (const listener of [...set]) {
      try {
        (listener as Listener<Events[K]>)(value);
      } catch (err) {
        // a throwing listener must not break the client; surface it asynchronously
        setTimeout(() => {
          throw err;
        }, 0);
      }
    }
  }
}
