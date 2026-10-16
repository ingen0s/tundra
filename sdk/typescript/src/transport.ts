/**
 * Transports carry whole Tundra frames. A transport factory opens one
 * connection; the client calls it again for every reconnect.
 *
 * Two are built in: TCP for Node (see tcp.ts, loaded on demand so browser
 * bundles never pull in node:net) and WebSocket, which uses the global
 * WebSocket of browsers and Node 22+.
 */
import { FrameParser, decodeFrame } from "./codec.js";
import { ConnectionError } from "./errors.js";

export interface TransportHandlers {
  /** One complete frame (header and payload). */
  frame(bytes: Uint8Array): void;
  /** The connection ended. Called at most once, and never for a connection that failed to open. */
  close(error?: Error): void;
}

export interface Transport {
  /** Sends one complete frame. */
  send(frame: Uint8Array): void;
  /** Closes the connection; `handlers.close` follows. */
  close(): void;
  /** A short description for logs, such as "tcp://127.0.0.1:4242". */
  readonly description: string;
}

/** Opens a connection. Rejects if it cannot be opened. */
export type TransportFactory = (handlers: TransportHandlers) => Promise<Transport>;

/** TLS options passed to node:tls connect() (servername, ca, rejectUnauthorized, ...). */
export type TlsOptions = Record<string, unknown>;

export interface TcpOptions {
  host?: string;
  port?: number;
  /** true, or node:tls connect options, to connect with TLS. */
  tls?: boolean | TlsOptions;
  /** Milliseconds to wait for the connection to open (default 10000). */
  connectTimeoutMs?: number;
}

/** The parts of the WebSocket API the transport uses. */
export interface WebSocketLike {
  binaryType: string;
  readonly readyState: number;
  send(data: Uint8Array): void;
  close(code?: number, reason?: string): void;
  onopen: ((ev: unknown) => void) | null;
  onmessage: ((ev: { data: unknown }) => void) | null;
  onerror: ((ev: unknown) => void) | null;
  onclose: ((ev: { code?: number; reason?: string }) => void) | null;
}

export type WebSocketConstructor = new (url: string, protocols?: string | string[]) => WebSocketLike;

export interface WebSocketOptions {
  /** A WebSocket constructor, for runtimes without a global one (for example the `ws` package). */
  WebSocket?: WebSocketConstructor;
  /** Milliseconds to wait for the connection to open (default 10000). */
  connectTimeoutMs?: number;
}

/** A transport factory for TCP (Node only). node:net / node:tls load on first use. */
export function tcpTransport(options: TcpOptions = {}): TransportFactory {
  return async (handlers) => {
    const mod = (await import("./tcp.js")) as Partial<typeof import("./tcp.js")>;
    if (typeof mod.openTcp !== "function") throw new ConnectionError("TCP transport is not available in this environment");
    return mod.openTcp(options, handlers);
  };
}

/** A transport factory for WebSocket. `url` is like "ws://host:8080/ws" or "wss://host/ws". */
export function webSocketTransport(url: string, options: WebSocketOptions = {}): TransportFactory {
  return (handlers) =>
    new Promise<Transport>((resolve, reject) => {
      const Ctor =
        options.WebSocket ?? (globalThis as unknown as { WebSocket?: WebSocketConstructor }).WebSocket;
      if (!Ctor) {
        reject(new ConnectionError("no WebSocket implementation: pass options.WebSocket"));
        return;
      }
      let ws: WebSocketLike;
      try {
        ws = new Ctor(url);
      } catch (err) {
        reject(err instanceof Error ? err : new ConnectionError(String(err)));
        return;
      }
      ws.binaryType = "arraybuffer";
      let opened = false;
      let closed = false;
      const timer = setTimeout(() => {
        if (opened) return;
        closed = true;
        ws.onopen = ws.onmessage = ws.onerror = ws.onclose = null;
        try {
          ws.close();
        } catch {
          /* already closing */
        }
        reject(new ConnectionError(`timed out connecting to ${url}`));
      }, options.connectTimeoutMs ?? 10_000);

      const finish = (error?: Error): void => {
        if (closed) return;
        closed = true;
        clearTimeout(timer);
        ws.onopen = ws.onmessage = ws.onerror = ws.onclose = null;
        if (opened) handlers.close(error);
        else reject(error ?? new ConnectionError(`could not connect to ${url}`));
      };

      const transport: Transport = {
        description: url,
        send(frame) {
          if (closed || ws.readyState !== 1) throw new ConnectionError("not connected");
          ws.send(frame);
        },
        close() {
          if (closed) return;
          try {
            ws.close(1000);
          } catch {
            /* ignore */
          }
          finish();
        },
      };

      ws.onopen = () => {
        opened = true;
        clearTimeout(timer);
        resolve(transport);
      };
      ws.onmessage = (ev) => {
        if (closed) return;
        const data = ev.data;
        if (!(data instanceof ArrayBuffer)) {
          // the server only sends binary messages; anything else means a broken peer
          transport.close();
          return;
        }
        const bytes = new Uint8Array(data);
        try {
          decodeFrame(bytes); // checks version and that it holds exactly one frame
        } catch (err) {
          try {
            ws.close(1007);
          } catch {
            /* ignore */
          }
          finish(err instanceof Error ? err : undefined);
          return;
        }
        handlers.frame(bytes);
      };
      ws.onerror = () => finish(new ConnectionError(`WebSocket error on ${url}`));
      ws.onclose = (ev) =>
        finish(ev.code === 1000 || ev.code === undefined ? undefined : new ConnectionError(`WebSocket closed (${ev.code})`));
    });
}

/** Splits a TCP byte stream into frames; shared with tcp.ts. */
export { FrameParser };
