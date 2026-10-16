/**
 * Tundra client: registers, answers pings, acknowledges what it receives and
 * reconnects with session resume (catching up on anything missed) when the
 * connection drops. Runs in browsers (WebSocket) and Node (TCP, TLS or
 * WebSocket).
 */
import {
  type AgentEntry,
  type ChatPayload,
  type ClientMessage,
  type HealthResultMsg,
  type Message,
  type ServerMessage,
  type WhoEntry,
  decodeMessage,
  encodeFrame,
} from "./codec.js";
import { Emitter } from "./emitter.js";
import { AgentError, ConnectionError, DecodeError, RequestTimeoutError, TundraError } from "./errors.js";
import { ChatKind, ErrorCode, FATAL_ERRORS, MsgType, RegisterTag, RoomEventKind, UUID_LEN } from "./protocol.js";
import { AsyncQueue } from "./queue.js";
import {
  type TlsOptions,
  type Transport,
  type TransportFactory,
  type WebSocketConstructor,
  tcpTransport,
  webSocketTransport,
} from "./transport.js";

// ---------------------------------------------------------------- public types

/** A chat message. */
export interface Chat {
  id: bigint;
  /** Server timestamp in milliseconds since the epoch. */
  ts: number;
  /** The room for room messages, null for direct messages. */
  room: string | null;
  /** The room, or for direct messages the recipient (you). */
  target: string;
  sender: string;
  text: string;
  /** True for direct messages. */
  direct: boolean;
}

export interface AgentInfo {
  online: boolean;
  /** Parsed JSON when the capabilities are JSON, the raw string otherwise. */
  capabilities: unknown;
}

export interface RoomEvent {
  event: "joined" | "left";
  room: string;
  user: string;
}

export interface HealthResult {
  ts: number;
  status: number;
  latencyMs: number;
  uptimeMs: number;
  sent: number;
  received: number;
}

export type ClientState = "idle" | "connecting" | "connected" | "reconnecting" | "closed";

/** Events emitted by a Client (and Agent). Listen with `client.on(name, fn)`. */
export interface ClientEvents {
  /** Registered: a new session, or `resumed` when an existing one was resumed. */
  connected: { sessionId: string; resumed: boolean };
  /** The connection dropped; the client will try to reconnect if allowed. */
  disconnected: { error?: Error };
  /** About to try reconnecting after `delayMs`. */
  reconnecting: { attempt: number; delayMs: number };
  /** The client stopped: close() was called, or it gave up (`error` is set). */
  closed: { error?: TundraError };
  /** Every chat message, after de-duplication (also delivered by messages()). */
  chat: Chat;
  roomEvent: RoomEvent;
  /** A SYSTEM notice (not the welcome). */
  system: { text: string };
  /** An ERROR that answers no pending call. */
  error: TundraError;
  /** A legacy BROADCAST_AGENT frame. */
  broadcast: { sender: string; ts: number; text: string };
  /** Every decoded frame from the server, before it is handled. */
  frame: { seq: number; message: ServerMessage };
}

export interface ClientOptions {
  username: string;
  /** Authentication token for this username. */
  token?: string;
  /** TCP host (Node). Default "127.0.0.1". */
  host?: string;
  /** TCP port (Node). Default 4242. */
  port?: number;
  /** TCP only: true, or node:tls connect options, to use TLS. */
  tls?: boolean | TlsOptions;
  /** A WebSocket URL such as "ws://localhost:8080/ws". Selects the WebSocket transport. */
  url?: string;
  /** WebSocket constructor for runtimes without a global WebSocket. */
  WebSocket?: WebSocketConstructor;
  /** A custom transport; overrides host/port/url. */
  transport?: TransportFactory;
  /** Reconnect and resume when the connection drops (default true). */
  reconnect?: boolean;
  /** How long to wait for a reply to a call such as send() or history() (default 10000 ms). */
  callTimeoutMs?: number;
  /** How long to batch received messages before acknowledging them (default 200 ms). */
  ackDelayMs?: number;
  /** Most chat messages held for messages() before the oldest are dropped (default 10000). */
  maxBufferedMessages?: number;
  /** First reconnect delay (default 200 ms); doubles up to maxReconnectDelayMs. */
  reconnectDelayMs?: number;
  /** Longest reconnect delay (default 5000 ms). */
  maxReconnectDelayMs?: number;
}

export interface RequestOptions {
  /** How long to wait for each answer (default 120000 ms). */
  timeoutMs?: number;
}

export interface HistoryOptions {
  /** Most messages to return, 1 to 100 (default 50). */
  limit?: number;
  /** Only messages older than this id; 0 or omitted for the latest. */
  beforeId?: bigint | number;
}

// ---------------------------------------------------------------- internals

type ServerType = ServerMessage["type"];

interface PendingCall {
  expect: ReadonlySet<number>;
  onReply?: ((m: ServerMessage) => void) | undefined;
  resolve(m: ServerMessage): void;
  reject(e: unknown): void;
  timer?: ReturnType<typeof setTimeout>;
}

interface CallOptions {
  /** null waits forever; default callTimeoutMs. */
  timeoutMs?: number | null;
  beforeRegister?: boolean;
  /** Runs synchronously when the reply arrives, before later frames are handled. */
  onReply?: (m: ServerMessage) => void;
}

interface RoomWaiter {
  room: string;
  event: RoomEventKind;
  resolve(): void;
}

interface AgentAnswer {
  status: number;
  final: boolean;
  body: string;
}

const ZERO_ID = new Uint8Array(UUID_LEN);

function hex(b: Uint8Array): string {
  let s = "";
  for (const x of b) s += x.toString(16).padStart(2, "0");
  return `${s.slice(0, 8)}-${s.slice(8, 12)}-${s.slice(12, 16)}-${s.slice(16, 20)}-${s.slice(20)}`;
}

function toChat(m: ChatPayload): Chat {
  const direct = m.kind === ChatKind.DIRECT;
  return {
    id: m.id,
    ts: Number(m.ts),
    room: direct ? null : m.target,
    target: m.target,
    sender: m.sender,
    text: m.text,
    direct,
  };
}

function errorOf(m: ServerMessage): TundraError | null {
  return m.type === MsgType.ERROR ? new TundraError(m.code, m.text) : null;
}

const utf8 = new TextEncoder();

// ---------------------------------------------------------------- client

export class Client extends Emitter<ClientEvents> {
  readonly username: string;
  readonly options: Readonly<ClientOptions>;
  /** Newest chat id received; sent as RESUME_FROM when resuming. */
  lastId = 0n;
  /** True when the last agents() call got a truncated list. */
  agentsTruncated = false;
  /** Set when the client gives up (name taken, invalid name, unauthorized). */
  error: TundraError | null = null;
  /** Rooms this session is in, kept up to date from room events. */
  readonly joinedRooms = new Set<string>();

  protected transport: Transport | null = null;
  protected sessionBytes: Uint8Array | null = null;
  protected _state: ClientState = "idle";

  private readonly factory: TransportFactory;
  private gen = 0;
  private seq = 0;
  private nextReq = 1n;
  private closing = false;
  private inbox: AsyncQueue<Chat> = new AsyncQueue();
  private seen = new Set<bigint>();
  private readonly replies = new Map<number, PendingCall>();
  private readonly requests = new Map<bigint, AsyncQueue<AgentAnswer>>();
  private roomWaiters: RoomWaiter[] = [];
  private ackTimer: ReturnType<typeof setTimeout> | null = null;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private wakeReconnect: (() => void) | null = null;
  private everRegistered = false;

  constructor(options: ClientOptions) {
    super();
    if (!options || typeof options.username !== "string") throw new TypeError("options.username is required");
    this.options = { ...options };
    this.username = options.username;
    if (options.transport) this.factory = options.transport;
    else if (options.url)
      this.factory = webSocketTransport(options.url, options.WebSocket ? { WebSocket: options.WebSocket } : {});
    else {
      const tcp: { host?: string; port?: number; tls?: boolean | TlsOptions } = {};
      if (options.host !== undefined) tcp.host = options.host;
      if (options.port !== undefined) tcp.port = options.port;
      if (options.tls !== undefined) tcp.tls = options.tls;
      this.factory = tcpTransport(tcp);
    }
  }

  /** Creates a client and connects it. */
  static async connect<T extends new (options: never) => Client>(
    this: T,
    options: ConstructorParameters<T>[0],
  ): Promise<InstanceType<T>> {
    const c = new this(options) as InstanceType<T>;
    await c.connect();
    return c;
  }

  get state(): ClientState {
    return this._state;
  }

  get connected(): boolean {
    return this._state === "connected";
  }

  /** The session id as a UUID string, or null before the first registration. */
  get sessionId(): string | null {
    return this.sessionBytes ? hex(this.sessionBytes) : null;
  }

  // ------------------------------------------------------------ connection

  /**
   * Connects and registers (or resumes). Rejects with TundraError when the
   * server refuses; a refused client does not keep reconnecting in the background.
   */
  async connect(): Promise<void> {
    if (this._state === "connected") return;
    if (this._state === "connecting" || this._state === "reconnecting")
      throw new ConnectionError(`already ${this._state}`);
    this.closing = false;
    this.error = null;
    if (this.inbox.closed) this.inbox = new AsyncQueue();
    this._state = "connecting";
    try {
      try {
        await this.open();
      } catch (err) {
        // reconnecting after close(): the old session is gone, so start a fresh one
        if (!(err instanceof TundraError && err.code === ErrorCode.SESSION_EXPIRED)) throw err;
        await this.open();
      }
    } catch (err) {
      this.closing = true;
      this._state = "closed";
      this.dropTransport();
      throw err;
    }
  }

  /** Closes the connection and stops reconnecting. messages() ends. */
  async close(): Promise<void> {
    if (this._state === "closed" && this.closing) return;
    this.shutdown();
    this.inbox.end();
    this.emit("closed", {});
  }

  /** Resolves once the client is connected (immediately if it is). */
  async ready(): Promise<void> {
    if (this._state === "connected") return;
    if (this._state === "closed" || this._state === "idle") throw new ConnectionError("not connected");
    await new Promise<void>((resolve, reject) => {
      const offs = [
        this.on("connected", () => {
          offs.forEach((f) => f());
          resolve();
        }),
        this.on("closed", (e) => {
          offs.forEach((f) => f());
          reject(e.error ?? new ConnectionError("client closed"));
        }),
      ];
    });
  }

  /** Called after every successful registration; Agent advertises here. */
  protected onConnected(_resumed: boolean): void {}

  /** Called for every AGENT_REQUEST_IN; Agent handles requests here. */
  protected onAgentRequest(_sender: string, _handle: bigint, _body: string): void {}

  /** Opens a transport and registers; throws on failure (and closes what it opened). */
  private async open(): Promise<void> {
    const gen = ++this.gen;
    const transport = await this.factory({
      frame: (bytes) => this.onFrame(gen, bytes),
      close: (error) => this.onTransportClose(gen, error),
    });
    if (gen !== this.gen || this.closing) {
      transport.close();
      throw new ConnectionError("client closed");
    }
    this.transport = transport;

    const extensions = [];
    if (this.sessionBytes && this.lastId > 0n) {
      const v = new Uint8Array(8);
      new DataView(v.buffer).setBigUint64(0, this.lastId);
      extensions.push({ tag: RegisterTag.RESUME_FROM, value: v });
    }
    if (this.options.token) extensions.push({ tag: RegisterTag.TOKEN, value: utf8.encode(this.options.token) });
    // The welcome is handled synchronously (onReply) because catch-up frames
    // can arrive in the same chunk, before an await continuation would run.
    await this.call(
      { type: MsgType.REGISTER, id: this.sessionBytes ?? ZERO_ID, username: this.username, extensions },
      [MsgType.SYSTEM, MsgType.ERROR],
      { timeoutMs: 10_000, beforeRegister: true, onReply: (m) => this.onWelcome(m) },
    ).catch((err: unknown) => {
      if (err instanceof TundraError && err.code === ErrorCode.SESSION_EXPIRED) this.sessionBytes = null;
      this.dropTransport();
      throw err;
    });
  }

  private onWelcome(reply: ServerMessage): void {
    if (reply.type !== MsgType.SYSTEM) return;
    const fresh = this.sessionBytes === null;
    const hadSession = this.everRegistered;
    this.sessionBytes = reply.id.slice();
    this.everRegistered = true;
    let rejoin: string[] = [];
    if (fresh) {
      // a new session: ids may restart (a server without a database), so forget what we saw
      this.seen.clear();
      this.lastId = 0n;
      if (hadSession) {
        // the old session expired: rejoin its rooms; its open requests will never be answered
        rejoin = [...this.joinedRooms].filter((r) => r !== "#lobby");
        this.joinedRooms.clear();
        for (const q of this.requests.values()) q.end(new ConnectionError("session expired"));
        this.requests.clear();
      }
    }
    this._state = "connected";
    this.onConnected(!fresh);
    for (const room of rejoin) this.join(room).catch(() => {});
    this.emit("connected", { sessionId: hex(this.sessionBytes), resumed: !fresh });
  }

  private onTransportClose(gen: number, error?: Error): void {
    if (gen !== this.gen) return;
    const wasConnected = this._state === "connected";
    this.transport = null;
    this.failReplies(new ConnectionError("connection lost"));
    if (!wasConnected) return; // open() is still running and sees the failure itself
    this.clearAck();
    this.emit("disconnected", error ? { error } : {});
    if (this.closing) return;
    if (this.options.reconnect === false) {
      this.shutdown();
      this.inbox.end();
      this.emit("closed", {});
      return;
    }
    this._state = "reconnecting";
    void this.reconnectLoop();
  }

  private async reconnectLoop(): Promise<void> {
    let delay = this.options.reconnectDelayMs ?? 200;
    const maxDelay = this.options.maxReconnectDelayMs ?? 5000;
    let wait = true;
    for (let attempt = 1; !this.closing; attempt++) {
      if (wait) {
        this.emit("reconnecting", { attempt, delayMs: delay });
        await new Promise<void>((resolve) => {
          this.wakeReconnect = resolve;
          this.reconnectTimer = setTimeout(resolve, delay);
        });
        this.reconnectTimer = null;
        this.wakeReconnect = null;
        if (this.closing) return;
      }
      try {
        await this.open();
        return;
      } catch (err) {
        if (this.closing) return;
        if (err instanceof TundraError && FATAL_ERRORS.has(err.code)) {
          this.giveUp(err);
          return;
        }
        // an expired session registers fresh right away; anything else backs off
        wait = !(err instanceof TundraError && err.code === ErrorCode.SESSION_EXPIRED);
        if (wait) delay = Math.min(delay * 2, maxDelay);
      }
    }
  }

  /** Retrying cannot help: stop, and let messages() fail with the error. */
  private giveUp(err: TundraError): void {
    this.error = err;
    this.shutdown();
    this.inbox.end(err);
    this.emit("closed", { error: err });
  }

  private shutdown(): void {
    this.closing = true;
    this._state = "closed";
    this.gen++;
    this.clearAck();
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
    this.wakeReconnect?.();
    this.wakeReconnect = null;
    this.dropTransport();
    this.failReplies(new ConnectionError("client closed"));
    for (const q of this.requests.values()) q.end(this.error ?? new ConnectionError("client closed"));
    this.requests.clear();
  }

  private dropTransport(): void {
    const t = this.transport;
    this.transport = null;
    t?.close();
  }

  private failReplies(err: Error): void {
    for (const p of this.replies.values()) {
      if (p.timer !== undefined) clearTimeout(p.timer);
      p.reject(err);
    }
    this.replies.clear();
    this.roomWaiters = [];
  }

  // ------------------------------------------------------------ frames

  /** Sends one message; returns its sequence number. */
  protected sendMessage(m: ClientMessage, beforeRegister = false): number {
    if (!this.transport || (!beforeRegister && this._state !== "connected")) throw new ConnectionError("not connected");
    const seq = this.seq & 0xffff;
    const bytes = encodeFrame(m, seq);
    this.seq = (this.seq + 1) & 0xffff;
    this.transport.send(bytes);
    return seq;
  }

  /** Sends a message and waits for a reply of one of the `expect` types; ERROR replies reject. */
  private call(
    m: ClientMessage,
    expect: readonly ServerType[],
    opts: CallOptions = {},
  ): Promise<ServerMessage> {
    return this.callRaw(m, expect, opts).promise.then((reply) => {
      const err = errorOf(reply);
      if (err) throw err;
      return reply;
    });
  }

  private callRaw(
    m: ClientMessage,
    expect: readonly ServerType[],
    opts: CallOptions = {},
  ): { promise: Promise<ServerMessage>; seq: number } {
    let seq = -1;
    const promise = new Promise<ServerMessage>((resolve, reject) => {
      seq = this.sendMessage(m, opts.beforeRegister);
      const p: PendingCall = { expect: new Set(expect), onReply: opts.onReply, resolve, reject };
      const timeout = opts.timeoutMs === undefined ? (this.options.callTimeoutMs ?? 10_000) : opts.timeoutMs;
      if (timeout !== null)
        p.timer = setTimeout(() => {
          if (this.replies.get(seq) === p) this.replies.delete(seq);
          reject(new RequestTimeoutError(`no reply to ${MsgType[m.type]} within ${timeout} ms`));
        }, timeout);
      this.replies.set(seq, p);
    });
    return { promise, seq };
  }

  private cancelCall(seq: number): void {
    const p = this.replies.get(seq);
    if (!p) return;
    this.replies.delete(seq);
    if (p.timer !== undefined) clearTimeout(p.timer);
  }

  private onFrame(gen: number, bytes: Uint8Array): void {
    if (gen !== this.gen) return;
    let seq: number;
    let msg: Message;
    try {
      const d = decodeMessage(bytes);
      seq = d.frame.seq;
      msg = d.message;
    } catch (err) {
      if (err instanceof DecodeError) return; // unknown or malformed: ignore, as the spec allows new types
      throw err;
    }
    if (msg.type < 0x10 || (msg.type >= 0x20 && msg.type < 0x30)) return; // a client type: not for us
    const m = msg as ServerMessage;
    this.emit("frame", { seq, message: m });

    const pending = this.replies.get(seq);
    if (pending && pending.expect.has(m.type)) {
      this.replies.delete(seq);
      if (pending.timer !== undefined) clearTimeout(pending.timer);
      pending.onReply?.(m);
      pending.resolve(m);
      return;
    }

    switch (m.type) {
      case MsgType.PING:
        this.trySend({ type: MsgType.PONG, data: bytes.subarray(7) });
        break;
      case MsgType.CHAT:
        this.onChat(m);
        break;
      case MsgType.AGENT_RESPONSE_IN:
        this.requests.get(m.reqId)?.push({ status: m.status, final: m.final, body: m.body });
        break;
      case MsgType.AGENT_REQUEST_IN:
        this.onAgentRequest(m.requester, m.handle, m.body);
        break;
      case MsgType.ROOM_EVENT: {
        const ev: RoomEvent = { event: m.event === RoomEventKind.JOINED ? "joined" : "left", room: m.room, user: m.user };
        if (m.user === this.username) {
          if (m.event === RoomEventKind.JOINED) this.joinedRooms.add(m.room);
          else this.joinedRooms.delete(m.room);
          const done = this.roomWaiters.filter((w) => w.room === m.room && w.event === m.event);
          this.roomWaiters = this.roomWaiters.filter((w) => !done.includes(w));
          for (const w of done) w.resolve();
        }
        this.emit("roomEvent", ev);
        break;
      }
      case MsgType.SYSTEM:
        this.emit("system", { text: m.text });
        break;
      case MsgType.BROADCAST_AGENT:
      case MsgType.BROADCAST_MESSAGE:
        this.emit("broadcast", { sender: m.sender, ts: Number(m.ts), text: m.text });
        break;
      case MsgType.ERROR: {
        const err = new TundraError(m.code, m.text);
        if (m.code === ErrorCode.UNAUTHORIZED) {
          // a token revoked on reload: the server disconnects us and retrying cannot help
          this.giveUp(err);
          return;
        }
        this.emit("error", err);
        break;
      }
      default:
        break; // unmatched replies are ignored
    }
  }

  private onChat(m: ChatPayload): void {
    if (this.seen.has(m.id)) return; // repeated by a catch-up replay
    this.seen.add(m.id);
    if (this.seen.size > 10_000) {
      const keep = this.lastId - 5000n;
      for (const id of this.seen) if (id <= keep) this.seen.delete(id);
    }
    if (m.id > this.lastId) this.lastId = m.id;
    this.scheduleAck();
    const chat = toChat(m);
    const max = this.options.maxBufferedMessages ?? 10_000;
    if (this.inbox.size >= max) void this.inbox.take(); // drop the oldest
    this.inbox.push(chat);
    this.emit("chat", chat);
  }

  private trySend(m: ClientMessage): void {
    try {
      this.sendMessage(m);
    } catch {
      /* the connection is going away; resume handles the rest */
    }
  }

  private scheduleAck(): void {
    if (this.ackTimer) return;
    this.ackTimer = setTimeout(() => {
      this.ackTimer = null;
      if (this._state === "connected" && this.lastId > 0n) this.trySend({ type: MsgType.ACK, id: this.lastId });
    }, this.options.ackDelayMs ?? 200);
  }

  private clearAck(): void {
    if (this.ackTimer) clearTimeout(this.ackTimer);
    this.ackTimer = null;
  }

  private waitRoomEvent(room: string, event: RoomEventKind, m: ClientMessage): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      const { promise, seq } = this.callRaw(m, [MsgType.ERROR]);
      const waiter: RoomWaiter = {
        room,
        event,
        resolve: () => {
          this.cancelCall(seq);
          resolve();
        },
      };
      this.roomWaiters.push(waiter);
      promise.then(
        (reply) => {
          this.roomWaiters = this.roomWaiters.filter((w) => w !== waiter);
          reject(errorOf(reply) ?? new ConnectionError("unexpected reply"));
        },
        (err: unknown) => {
          this.roomWaiters = this.roomWaiters.filter((w) => w !== waiter);
          reject(err);
        },
      );
    });
  }

  // ------------------------------------------------------------ chat

  /** Joins a room (created if needed). Resolves once the server confirms. */
  join(room: string): Promise<void> {
    return this.waitRoomEvent(room, RoomEventKind.JOINED, { type: MsgType.JOIN, room });
  }

  /** Leaves a room. Resolves once the server confirms. */
  leave(room: string): Promise<void> {
    return this.waitRoomEvent(room, RoomEventKind.LEFT, { type: MsgType.LEAVE, room });
  }

  /** Sends to a "#room" or a username. Resolves with the message id. */
  async send(target: string, text: string): Promise<bigint> {
    const kind = target.startsWith("#") ? ChatKind.ROOM : ChatKind.DIRECT;
    const reply = await this.call({ type: MsgType.SEND, kind, target, text }, [MsgType.SENT, MsgType.ERROR]);
    if (reply.type !== MsgType.SENT) throw new ConnectionError("unexpected reply");
    return reply.id;
  }

  /** Recent messages in a room you are in, oldest first. */
  async history(room: string, options: HistoryOptions = {}): Promise<Chat[]> {
    const beforeId = BigInt(options.beforeId ?? 0);
    const limit = options.limit ?? 50;
    const reply = await this.call({ type: MsgType.HISTORY, room, beforeId, limit }, [MsgType.HISTORY_RESULT, MsgType.ERROR]);
    if (reply.type !== MsgType.HISTORY_RESULT) throw new ConnectionError("unexpected reply");
    return reply.messages.map(toChat);
  }

  /** Every room and its member count. */
  async rooms(): Promise<Record<string, number>> {
    const reply = await this.call({ type: MsgType.LIST_ROOMS }, [MsgType.ROOM_LIST, MsgType.ERROR]);
    if (reply.type !== MsgType.ROOM_LIST) throw new ConnectionError("unexpected reply");
    const out: Record<string, number> = {};
    for (const r of reply.rooms) out[r.room] = r.members;
    return out;
  }

  /** The members of a room and whether each is online. */
  async who(room: string): Promise<WhoEntry[]> {
    const reply = await this.call({ type: MsgType.WHO, room }, [MsgType.WHO_RESULT, MsgType.ERROR]);
    if (reply.type !== MsgType.WHO_RESULT) throw new ConnectionError("unexpected reply");
    return reply.members;
  }

  /** Latency, uptime and frame counters for this session. */
  async health(): Promise<HealthResult> {
    const reply = await this.call({ type: MsgType.HEALTH_CHECK }, [MsgType.HEALTH_RESULT, MsgType.ERROR]);
    if (reply.type !== MsgType.HEALTH_RESULT) throw new ConnectionError("unexpected reply");
    const h: HealthResultMsg = reply;
    return { ts: Number(h.ts), status: h.status, latencyMs: h.latencyMs, uptimeMs: h.uptimeMs, sent: h.sent, received: h.received };
  }

  /**
   * Every chat message received, including catch-up after a resume. Messages
   * are buffered from the moment the client connects, and each is delivered
   * to one messages() iterator. Ends when the client is closed; throws the
   * error when the client gives up reconnecting (see `error`).
   */
  messages(): AsyncIterableIterator<Chat> {
    return this.inbox.iterator();
  }

  /**
   * The next chat message from the same buffer messages() reads, or null if
   * none arrives within `timeoutMs` (or the client closes). Throws the error
   * when the client gives up.
   */
  async nextMessage(timeoutMs?: number): Promise<Chat | null> {
    const r = await this.inbox.take(timeoutMs);
    return r.done ? null : r.value;
  }

  // ------------------------------------------------------------ agents

  /** Every advertised agent: { name: { online, capabilities } }. Sets `agentsTruncated`. */
  async agents(): Promise<Record<string, AgentInfo>> {
    const reply = await this.call({ type: MsgType.LIST_AGENTS }, [MsgType.AGENT_LIST, MsgType.ERROR]);
    if (reply.type !== MsgType.AGENT_LIST) throw new ConnectionError("unexpected reply");
    this.agentsTruncated = reply.truncated; // more agents exist than fit in one frame
    const out: Record<string, AgentInfo> = {};
    for (const a of reply.agents as AgentEntry[]) {
      let caps: unknown = a.capabilities;
      try {
        caps = JSON.parse(a.capabilities);
      } catch {
        /* not JSON: keep the string */
      }
      out[a.name] = { online: a.online, capabilities: caps };
    }
    return out;
  }

  /**
   * Sends a request and yields each answer (partial ones first, the final one
   * last). Throws AgentError when the agent or server answers with a non-ok
   * status, TundraError when the server refuses the request (unknown agent,
   * not an agent), and RequestTimeoutError when no answer comes in time.
   */
  async *requestStream(agent: string, body: string, options: RequestOptions = {}): AsyncGenerator<string, void, undefined> {
    const timeoutMs = options.timeoutMs ?? 120_000;
    const rid = this.nextReq++;
    const q = new AsyncQueue<AgentAnswer>();
    this.requests.set(rid, q);
    let seq = -1;
    try {
      // A refused request comes back as an ERROR reply to the request's sequence number.
      const c = this.callRaw({ type: MsgType.AGENT_REQUEST, agent, reqId: rid, body }, [MsgType.ERROR], { timeoutMs: null });
      seq = c.seq;
      c.promise.then(
        (reply) => q.end(errorOf(reply) ?? new ConnectionError("unexpected reply")),
        () => {
          /* connection lost: answers still arrive after a resume */
        },
      );
      for (;;) {
        const r = await q.take(timeoutMs);
        if (r.done) throw new RequestTimeoutError(`no answer from ${agent} within ${timeoutMs} ms`);
        this.cancelCall(seq); // the request was accepted
        const a = r.value;
        if (a.status !== 0) throw new AgentError(agent, a.status, a.body);
        yield a.body;
        if (a.final) return;
      }
    } finally {
      this.requests.delete(rid);
      if (seq >= 0) this.cancelCall(seq);
    }
  }

  /** Sends a request and resolves with the final answer. */
  async request(agent: string, body: string, options: RequestOptions = {}): Promise<string> {
    let last = "";
    for await (const part of this.requestStream(agent, body, options)) last = part;
    return last;
  }
}
