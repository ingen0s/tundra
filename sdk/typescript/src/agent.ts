import { Client, type ClientOptions } from "./client.js";
import { AgentError } from "./errors.js";
import { AgentStatus, MsgType } from "./protocol.js";

/** A request delivered to an agent. */
export interface AgentRequest {
  /** The requester's username. */
  sender: string;
  /** The server-assigned handle; answers are matched to it. */
  id: bigint;
  body: string;
  /** Aborted when the agent closes or loses its session; long handlers can check it. */
  signal: AbortSignal;
}

/** What a handler may return: an answer, or an (async) iterable of partial answers, the last one final. */
export type HandlerResult =
  | string
  | number
  | null
  | undefined
  | void
  | Iterable<unknown>
  | AsyncIterable<unknown>
  | Promise<string | number | null | undefined | void>;

/**
 * Handles one request. Return (or resolve) the answer, or be a generator /
 * async generator that yields partial answers. Throwing answers with status
 * ERROR; throwing an AgentError answers with its status and body.
 */
export type AgentHandler = (req: AgentRequest) => HandlerResult;

export interface AgentOptions extends ClientOptions {
  /** Advertised capabilities: an object (sent as JSON) or a string. Default {}. */
  capabilities?: unknown;
  handler?: AgentHandler;
}

function isAsyncIterable(v: unknown): v is AsyncIterable<unknown> {
  return v !== null && typeof v === "object" && Symbol.asyncIterator in v;
}

function isIterable(v: unknown): v is Iterable<unknown> {
  return v !== null && typeof v === "object" && Symbol.iterator in v && typeof v !== "string";
}

function text(v: unknown): string {
  return v === null || v === undefined ? "" : typeof v === "string" ? v : String(v);
}

/**
 * A client that advertises capabilities and answers requests with a handler.
 * It re-advertises after every registration, so it keeps working across
 * reconnects and expired sessions.
 */
export class Agent extends Client {
  capabilities: unknown;
  private handlerFn: AgentHandler | null;
  private aborter = new AbortController();
  /** Answers produced while reconnecting, sent once the session resumes. */
  private unsent: { req: AgentRequest; status: number; final: boolean; body: string }[] = [];
  /** Most answers held while reconnecting; older ones are dropped beyond this. */
  static maxUnsent = 4096;

  constructor(options: AgentOptions) {
    super(options);
    this.capabilities = options.capabilities ?? {};
    this.handlerFn = options.handler ?? null;
    // however the client stops (close(), failed connect, giving up), outstanding work is void
    this.on("closed", () => {
      this.aborter.abort();
      this.unsent = [];
    });
  }

  /** Sets the request handler. Returns it, so it can wrap a function definition. */
  handler<H extends AgentHandler>(fn: H): H {
    this.handlerFn = fn;
    return fn;
  }

  /** Connects and stays connected until close(). */
  async runForever(): Promise<void> {
    await this.connect();
    await new Promise<void>((resolve, reject) => {
      this.once("closed", (e) => (e.error ? reject(e.error) : resolve()));
    });
  }

  /** Changes the advertised capabilities (re-advertises when connected). */
  advertise(capabilities: unknown): void {
    this.capabilities = capabilities;
    if (this.connected) this.sendAdvertise();
  }

  protected override onConnected(resumed: boolean): void {
    if (!resumed || this.aborter.signal.aborted) {
      // a fresh session cannot answer requests given to an old one; after close() start over
      this.aborter.abort();
      this.aborter = new AbortController();
      this.unsent = [];
    }
    this.sendAdvertise();
    // the session resumed: the server still holds those requests open, so deliver the answers
    const queued = this.unsent;
    this.unsent = [];
    for (const r of queued) this.respond(r.req, r.status, r.final, r.body);
  }

  private sendAdvertise(): void {
    const caps = typeof this.capabilities === "string" ? this.capabilities : JSON.stringify(this.capabilities ?? {});
    try {
      this.sendMessage({ type: MsgType.AGENT_ADVERTISE, capabilities: caps || "{}" });
    } catch {
      /* not connected: advertised again on the next registration */
    }
  }

  protected override onAgentRequest(sender: string, handle: bigint, body: string): void {
    void this.handle({ sender, id: handle, body, signal: this.aborter.signal });
  }

  private respond(req: AgentRequest, status: number, final: boolean, body: string): void {
    if (req.signal.aborted) return;
    if (this.connected && this.unsent.length === 0) {
      try {
        this.sendMessage({ type: MsgType.AGENT_RESPONSE, requester: req.sender, handle: req.id, status, final, body });
        return;
      } catch {
        /* the connection just dropped: hold the answer for the resumed session */
      }
    }
    if (this.state === "closed") return;
    // Reconnecting: keep it until the session resumes. If the session turns out
    // to be fresh, the server has already answered the requester UNAVAILABLE.
    this.unsent.push({ req, status, final, body });
    if (this.unsent.length > Agent.maxUnsent) this.unsent.shift();
  }

  private async handle(req: AgentRequest): Promise<void> {
    const fn = this.handlerFn;
    if (!fn) {
      this.respond(req, AgentStatus.REJECTED, true, "no handler");
      return;
    }
    try {
      const result = fn(req);
      if (isAsyncIterable(result) || isIterable(result)) {
        let prev: unknown;
        let have = false;
        for await (const part of result as AsyncIterable<unknown>) {
          if (have) this.respond(req, AgentStatus.OK, false, text(prev));
          prev = part;
          have = true;
        }
        this.respond(req, AgentStatus.OK, true, have ? text(prev) : "");
      } else {
        this.respond(req, AgentStatus.OK, true, text(await result));
      }
    } catch (err) {
      // report the failure to the requester
      if (err instanceof AgentError) this.respond(req, err.status, true, err.body);
      else if (err instanceof Error) this.respond(req, AgentStatus.ERROR, true, `${err.name}: ${err.message}`);
      else this.respond(req, AgentStatus.ERROR, true, String(err));
    }
  }
}
