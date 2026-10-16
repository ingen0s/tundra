/// <reference types="node" />
/**
 * TCP and TLS transport for Node. This module is only ever loaded through a
 * dynamic import from transport.ts, and loads node:net / node:tls lazily
 * itself, so browser bundles can leave it out (package.json maps it to
 * false under "browser").
 */
import { FrameParser } from "./codec.js";
import { ConnectionError } from "./errors.js";
import type { TcpOptions, Transport, TransportHandlers } from "./transport.js";

interface SocketLike {
  setNoDelay(on: boolean): void;
  write(data: Uint8Array): boolean;
  end(): void;
  destroy(error?: Error): void;
  readonly destroyed: boolean;
  on(event: "data", fn: (chunk: Uint8Array) => void): void;
  on(event: "error", fn: (err: Error) => void): void;
  on(event: "close", fn: () => void): void;
  once(event: string, fn: (...args: unknown[]) => void): void;
  removeAllListeners(event?: string): void;
}

export async function openTcp(options: TcpOptions, handlers: TransportHandlers): Promise<Transport> {
  const host = options.host ?? "127.0.0.1";
  const port = options.port ?? 4242;
  const useTls = options.tls !== undefined && options.tls !== false;
  const scheme = useTls ? "tls" : "tcp";
  const description = `${scheme}://${host}:${port}`;

  let socket: SocketLike;
  let readyEvent: string;
  if (useTls) {
    const tls = await import("node:tls");
    const extra = typeof options.tls === "object" ? options.tls : {};
    socket = tls.connect({ servername: isIp(host) ? undefined : host, ...extra, host, port }) as unknown as SocketLike;
    readyEvent = "secureConnect";
  } else {
    const net = await import("node:net");
    socket = net.connect({ host, port }) as unknown as SocketLike;
    readyEvent = "connect";
  }

  await new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => {
      socket.destroy();
      reject(new ConnectionError(`timed out connecting to ${description}`));
    }, options.connectTimeoutMs ?? 10_000);
    socket.once(readyEvent, () => {
      clearTimeout(timer);
      socket.removeAllListeners("error");
      resolve();
    });
    socket.once("error", (err) => {
      clearTimeout(timer);
      socket.destroy();
      reject(err instanceof Error ? err : new ConnectionError(String(err)));
    });
  });

  socket.setNoDelay(true);
  const parser = new FrameParser();
  let closed = false;
  let lastError: Error | undefined;

  socket.on("data", (chunk) => {
    if (closed) return;
    let frames: Uint8Array[];
    try {
      frames = parser.push(chunk);
    } catch (err) {
      lastError = err instanceof Error ? err : new ConnectionError(String(err));
      socket.destroy();
      return;
    }
    for (const f of frames) {
      if (closed) return;
      handlers.frame(f);
    }
  });
  socket.on("error", (err) => {
    lastError = err;
  });
  socket.on("close", () => {
    if (closed) return;
    closed = true;
    handlers.close(lastError);
  });

  return {
    description,
    send(frame) {
      if (closed || socket.destroyed) throw new ConnectionError("not connected");
      socket.write(frame);
    },
    close() {
      if (closed) return;
      closed = true;
      socket.end();
      socket.destroy();
      handlers.close();
    },
  };
}

function isIp(host: string): boolean {
  return /^[\d.]+$/.test(host) || host.includes(":");
}
