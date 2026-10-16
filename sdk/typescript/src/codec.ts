/**
 * Encode and decode every Tundra message type.
 *
 * Decoding is bounds-checked and strict like the C reference codec: every
 * payload must be exactly the size its fields describe, otherwise a
 * DecodeError is thrown. 64-bit fields (ids, timestamps, handles) are BigInt.
 * Text fields are decoded as UTF-8 with replacement characters for invalid
 * bytes. Works in browsers and Node: only Uint8Array, DataView and
 * TextEncoder/TextDecoder are used.
 */
import { DecodeError } from "./errors.js";
import { ChatKind, HEADER_LEN, MAX_PAYLOAD, MsgType, RoomEventKind, UUID_LEN, VERSION } from "./protocol.js";

const utf8 = new TextEncoder();
const utf8d = new TextDecoder();
const U64_MAX = (1n << 64n) - 1n;

/** A string, or bytes sent as they are. */
export type Text = string | Uint8Array;

// ---------------------------------------------------------------- types

export interface RegisterExtension {
  tag: number;
  value: Uint8Array;
}

/** The fields of a CHAT payload; also each HISTORY_RESULT entry. */
export interface ChatPayload {
  id: bigint;
  ts: bigint;
  kind: ChatKind | number;
  target: string;
  sender: string;
  text: string;
}

export interface RoomEntry {
  room: string;
  members: number;
}

export interface WhoEntry {
  user: string;
  online: boolean;
}

export interface AgentEntry {
  name: string;
  online: boolean;
  capabilities: string;
}

export type RegisterMsg = { type: MsgType.REGISTER; id: Uint8Array; username: string; extensions: RegisterExtension[] };
export type MessageMsg = { type: MsgType.MESSAGE; text: string };
export type AgentCommandMsg = { type: MsgType.AGENT_COMMAND; agent: string; command: string };
export type PongMsg = { type: MsgType.PONG; data: Uint8Array };
export type HealthCheckMsg = { type: MsgType.HEALTH_CHECK };
export type JoinMsg = { type: MsgType.JOIN; room: string };
export type LeaveMsg = { type: MsgType.LEAVE; room: string };
export type SendMsg = { type: MsgType.SEND; kind: ChatKind | number; target: string; text: string };
export type ListRoomsMsg = { type: MsgType.LIST_ROOMS };
export type AckMsg = { type: MsgType.ACK; id: bigint };
export type HistoryMsg = { type: MsgType.HISTORY; room: string; beforeId: bigint; limit: number };
export type WhoMsg = { type: MsgType.WHO; room: string };
export type AgentAdvertiseMsg = { type: MsgType.AGENT_ADVERTISE; capabilities: string };
export type AgentRequestMsg = { type: MsgType.AGENT_REQUEST; agent: string; reqId: bigint; body: string };
export type AgentResponseMsg = {
  type: MsgType.AGENT_RESPONSE;
  requester: string;
  handle: bigint;
  status: number;
  final: boolean;
  body: string;
};
export type ListAgentsMsg = { type: MsgType.LIST_AGENTS };

export type SystemMsg = { type: MsgType.SYSTEM; id: Uint8Array; text: string };
export type BroadcastMsg = {
  type: MsgType.BROADCAST_MESSAGE | MsgType.BROADCAST_AGENT;
  sender: string;
  ts: bigint;
  text: string;
};
export type PingMsg = { type: MsgType.PING; ts: bigint };
export type ErrorMsg = { type: MsgType.ERROR; code: number; text: string };
export type HealthResultMsg = {
  type: MsgType.HEALTH_RESULT;
  ts: bigint;
  status: number;
  latencyMs: number;
  uptimeMs: number;
  sent: number;
  received: number;
};
export type ChatMsg = { type: MsgType.CHAT } & ChatPayload;
export type SentMsg = { type: MsgType.SENT; id: bigint };
export type RoomListMsg = { type: MsgType.ROOM_LIST; rooms: RoomEntry[] };
export type RoomEventMsg = { type: MsgType.ROOM_EVENT; event: RoomEventKind | number; room: string; user: string };
export type HistoryResultMsg = { type: MsgType.HISTORY_RESULT; room: string; messages: ChatPayload[] };
export type WhoResultMsg = { type: MsgType.WHO_RESULT; room: string; members: WhoEntry[] };
export type AgentListMsg = { type: MsgType.AGENT_LIST; truncated: boolean; agents: AgentEntry[] };
export type AgentRequestInMsg = { type: MsgType.AGENT_REQUEST_IN; requester: string; handle: bigint; body: string };
export type AgentResponseInMsg = {
  type: MsgType.AGENT_RESPONSE_IN;
  agent: string;
  reqId: bigint;
  status: number;
  final: boolean;
  body: string;
};

export type ClientMessage =
  | RegisterMsg
  | MessageMsg
  | AgentCommandMsg
  | PongMsg
  | HealthCheckMsg
  | JoinMsg
  | LeaveMsg
  | SendMsg
  | ListRoomsMsg
  | AckMsg
  | HistoryMsg
  | WhoMsg
  | AgentAdvertiseMsg
  | AgentRequestMsg
  | AgentResponseMsg
  | ListAgentsMsg;

export type ServerMessage =
  | SystemMsg
  | BroadcastMsg
  | PingMsg
  | ErrorMsg
  | HealthResultMsg
  | ChatMsg
  | SentMsg
  | RoomListMsg
  | RoomEventMsg
  | HistoryResultMsg
  | WhoResultMsg
  | AgentListMsg
  | AgentRequestInMsg
  | AgentResponseInMsg;

/** Any message, with its payload fields. */
export type Message = ClientMessage | ServerMessage;

/** One frame: the header fields and the raw payload. */
export interface Frame {
  version: number;
  flags: number;
  seq: number;
  type: number;
  payload: Uint8Array;
}

// ---------------------------------------------------------------- writer

class Writer {
  private buf = new Uint8Array(64);
  private view = new DataView(this.buf.buffer);
  len = 0;

  private room(n: number): void {
    const need = this.len + n;
    if (need > MAX_PAYLOAD) throw new RangeError(`payload longer than ${MAX_PAYLOAD} bytes`);
    if (need <= this.buf.length) return;
    let size = this.buf.length * 2;
    while (size < need) size *= 2;
    const next = new Uint8Array(Math.min(size, MAX_PAYLOAD));
    next.set(this.buf.subarray(0, this.len));
    this.buf = next;
    this.view = new DataView(next.buffer);
  }

  u8(v: number): this {
    checkInt(v, 0xff, "u8");
    this.room(1);
    this.buf[this.len++] = v;
    return this;
  }

  u16(v: number): this {
    checkInt(v, 0xffff, "u16");
    this.room(2);
    this.view.setUint16(this.len, v);
    this.len += 2;
    return this;
  }

  u32(v: number): this {
    checkInt(v, 0xffffffff, "u32");
    this.room(4);
    this.view.setUint32(this.len, v);
    this.len += 4;
    return this;
  }

  u64(v: bigint | number): this {
    const b = typeof v === "bigint" ? v : Number.isSafeInteger(v) ? BigInt(v) : NaN;
    if (typeof b !== "bigint" || b < 0n || b > U64_MAX) throw new RangeError(`u64 out of range: ${String(v)}`);
    this.room(8);
    this.view.setBigUint64(this.len, b);
    this.len += 8;
    return this;
  }

  bool(v: boolean): this {
    return this.u8(v ? 1 : 0);
  }

  bytes(b: Uint8Array): this {
    this.room(b.length);
    this.buf.set(b, this.len);
    this.len += b.length;
    return this;
  }

  str8(s: Text): this {
    const b = toBytes(s);
    if (b.length > 0xff) throw new RangeError("field longer than 255 bytes");
    return this.u8(b.length).bytes(b);
  }

  str16(s: Text): this {
    const b = toBytes(s);
    if (b.length > 0xffff) throw new RangeError("field longer than 65535 bytes");
    return this.u16(b.length).bytes(b);
  }

  done(): Uint8Array {
    return this.buf.slice(0, this.len);
  }
}

function checkInt(v: number, max: number, what: string): void {
  if (!Number.isInteger(v) || v < 0 || v > max) throw new RangeError(`${what} out of range: ${String(v)}`);
}

function toBytes(s: Text): Uint8Array {
  return typeof s === "string" ? utf8.encode(s) : s;
}

// ---------------------------------------------------------------- reader

class Reader {
  private readonly view: DataView;
  pos = 0;

  constructor(private readonly b: Uint8Array) {
    this.view = new DataView(b.buffer, b.byteOffset, b.byteLength);
  }

  private need(n: number): number {
    if (this.pos + n > this.b.length) throw new DecodeError("truncated payload");
    const at = this.pos;
    this.pos += n;
    return at;
  }

  u8(): number {
    return this.b[this.need(1)] as number;
  }

  u16(): number {
    return this.view.getUint16(this.need(2));
  }

  u32(): number {
    return this.view.getUint32(this.need(4));
  }

  u64(): bigint {
    return this.view.getBigUint64(this.need(8));
  }

  bool(): boolean {
    return this.u8() !== 0;
  }

  raw(n: number): Uint8Array {
    const at = this.need(n);
    return this.b.subarray(at, at + n);
  }

  str8(): string {
    return utf8d.decode(this.raw(this.u8()));
  }

  str16(): string {
    return utf8d.decode(this.raw(this.u16()));
  }

  left(): number {
    return this.b.length - this.pos;
  }

  rest(): Uint8Array {
    return this.raw(this.left());
  }

  done(): void {
    if (this.pos !== this.b.length) throw new DecodeError("trailing bytes in payload");
  }
}

// ---------------------------------------------------------------- payloads

function writeChat(w: Writer, m: ChatPayload): void {
  w.u64(m.id).u64(m.ts).u8(m.kind).str8(m.target).str8(m.sender).str16(m.text);
}

function readChat(r: Reader): ChatPayload {
  const id = r.u64();
  const ts = r.u64();
  const kind = r.u8();
  const target = r.str8();
  const sender = r.str8();
  const text = r.str16();
  return { id, ts, kind, target, sender, text };
}

function checkId16(id: Uint8Array): Uint8Array {
  if (id.length !== UUID_LEN) throw new RangeError(`id must be ${UUID_LEN} bytes`);
  return id;
}

/** Encodes a message's payload (without the header). Throws RangeError for values that do not fit. */
export function encodePayload(m: Message): Uint8Array {
  const w = new Writer();
  switch (m.type) {
    case MsgType.REGISTER:
      w.bytes(checkId16(m.id)).str8(m.username);
      for (const e of m.extensions) w.u8(e.tag).str16(e.value);
      break;
    case MsgType.MESSAGE:
      w.str16(m.text);
      break;
    case MsgType.AGENT_COMMAND:
      w.str8(m.agent).str16(m.command);
      break;
    case MsgType.PONG:
      w.bytes(m.data);
      break;
    case MsgType.HEALTH_CHECK:
    case MsgType.LIST_ROOMS:
    case MsgType.LIST_AGENTS:
      break;
    case MsgType.JOIN:
    case MsgType.LEAVE:
    case MsgType.WHO:
      w.str8(m.room);
      break;
    case MsgType.SEND:
      w.u8(m.kind).str8(m.target).str16(m.text);
      break;
    case MsgType.ACK:
    case MsgType.SENT:
      w.u64(m.id);
      break;
    case MsgType.HISTORY:
      w.str8(m.room).u64(m.beforeId).u16(m.limit);
      break;
    case MsgType.AGENT_ADVERTISE:
      w.str16(m.capabilities);
      break;
    case MsgType.AGENT_REQUEST:
      w.str8(m.agent).u64(m.reqId).str16(m.body);
      break;
    case MsgType.AGENT_RESPONSE:
      w.str8(m.requester).u64(m.handle).u8(m.status).bool(m.final).str16(m.body);
      break;
    case MsgType.SYSTEM:
      w.bytes(checkId16(m.id)).str16(m.text);
      break;
    case MsgType.BROADCAST_MESSAGE:
    case MsgType.BROADCAST_AGENT:
      w.str8(m.sender).u64(m.ts).str16(m.text);
      break;
    case MsgType.PING:
      w.u64(m.ts);
      break;
    case MsgType.ERROR:
      w.u16(m.code).str16(m.text);
      break;
    case MsgType.HEALTH_RESULT:
      w.u64(m.ts).u8(m.status).u32(m.latencyMs).u32(m.uptimeMs).u32(m.sent).u32(m.received);
      break;
    case MsgType.CHAT:
      writeChat(w, m);
      break;
    case MsgType.ROOM_LIST:
      w.u16(m.rooms.length);
      for (const e of m.rooms) w.str8(e.room).u16(e.members);
      break;
    case MsgType.ROOM_EVENT:
      w.u8(m.event).str8(m.room).str8(m.user);
      break;
    case MsgType.HISTORY_RESULT:
      w.str8(m.room).u16(m.messages.length);
      for (const c of m.messages) {
        const e = new Writer();
        writeChat(e, c);
        w.str16(e.done());
      }
      break;
    case MsgType.WHO_RESULT:
      w.str8(m.room).u16(m.members.length);
      for (const e of m.members) w.str8(e.user).bool(e.online);
      break;
    case MsgType.AGENT_LIST:
      w.bool(m.truncated).u16(m.agents.length);
      for (const a of m.agents) w.str8(a.name).bool(a.online).str16(a.capabilities);
      break;
    case MsgType.AGENT_REQUEST_IN:
      w.str8(m.requester).u64(m.handle).str16(m.body);
      break;
    case MsgType.AGENT_RESPONSE_IN:
      w.str8(m.agent).u64(m.reqId).u8(m.status).bool(m.final).str16(m.body);
      break;
    default: {
      const never: never = m;
      throw new RangeError(`unknown message type ${String((never as { type: unknown }).type)}`);
    }
  }
  return w.done();
}

/** Decodes a payload of the given type. Throws DecodeError if it is malformed or the type is unknown. */
export function decodePayload(type: number, payload: Uint8Array): Message {
  const r = new Reader(payload);
  let m: Message;
  switch (type) {
    case MsgType.REGISTER: {
      const id = r.raw(UUID_LEN).slice();
      const username = r.str8();
      const extensions: RegisterExtension[] = [];
      while (r.left() > 0) {
        const tag = r.u8();
        extensions.push({ tag, value: r.raw(r.u16()).slice() });
      }
      m = { type, id, username, extensions };
      break;
    }
    case MsgType.MESSAGE:
      m = { type, text: r.str16() };
      break;
    case MsgType.AGENT_COMMAND:
      m = { type, agent: r.str8(), command: r.str16() };
      break;
    case MsgType.PONG:
      m = { type, data: r.rest().slice() };
      break;
    case MsgType.HEALTH_CHECK:
    case MsgType.LIST_ROOMS:
    case MsgType.LIST_AGENTS:
      m = { type };
      break;
    case MsgType.JOIN:
    case MsgType.LEAVE:
    case MsgType.WHO:
      m = { type, room: r.str8() };
      break;
    case MsgType.SEND: {
      const kind = r.u8();
      const target = r.str8();
      m = { type, kind, target, text: r.str16() };
      break;
    }
    case MsgType.ACK:
    case MsgType.SENT:
      m = { type, id: r.u64() };
      break;
    case MsgType.HISTORY: {
      const room = r.str8();
      const beforeId = r.u64();
      m = { type, room, beforeId, limit: r.u16() };
      break;
    }
    case MsgType.AGENT_ADVERTISE:
      m = { type, capabilities: r.str16() };
      break;
    case MsgType.AGENT_REQUEST: {
      const agent = r.str8();
      const reqId = r.u64();
      m = { type, agent, reqId, body: r.str16() };
      break;
    }
    case MsgType.AGENT_RESPONSE: {
      const requester = r.str8();
      const handle = r.u64();
      const status = r.u8();
      const final = r.bool();
      m = { type, requester, handle, status, final, body: r.str16() };
      break;
    }
    case MsgType.SYSTEM: {
      const id = r.raw(UUID_LEN).slice();
      m = { type, id, text: r.str16() };
      break;
    }
    case MsgType.BROADCAST_MESSAGE:
    case MsgType.BROADCAST_AGENT: {
      const sender = r.str8();
      const ts = r.u64();
      m = { type, sender, ts, text: r.str16() };
      break;
    }
    case MsgType.PING:
      m = { type, ts: r.u64() };
      break;
    case MsgType.ERROR: {
      const code = r.u16();
      m = { type, code, text: r.str16() };
      break;
    }
    case MsgType.HEALTH_RESULT: {
      const ts = r.u64();
      const status = r.u8();
      const latencyMs = r.u32();
      const uptimeMs = r.u32();
      const sent = r.u32();
      m = { type, ts, status, latencyMs, uptimeMs, sent, received: r.u32() };
      break;
    }
    case MsgType.CHAT:
      m = { type, ...readChat(r) };
      break;
    case MsgType.ROOM_LIST: {
      const rooms: RoomEntry[] = [];
      for (let n = r.u16(); n > 0; n--) rooms.push({ room: r.str8(), members: r.u16() });
      m = { type, rooms };
      break;
    }
    case MsgType.ROOM_EVENT: {
      const event = r.u8();
      const room = r.str8();
      m = { type, event, room, user: r.str8() };
      break;
    }
    case MsgType.HISTORY_RESULT: {
      const room = r.str8();
      const messages: ChatPayload[] = [];
      for (let n = r.u16(); n > 0; n--) {
        const entry = new Reader(r.raw(r.u16()));
        messages.push(readChat(entry));
        entry.done();
      }
      m = { type, room, messages };
      break;
    }
    case MsgType.WHO_RESULT: {
      const room = r.str8();
      const members: WhoEntry[] = [];
      for (let n = r.u16(); n > 0; n--) members.push({ user: r.str8(), online: r.bool() });
      m = { type, room, members };
      break;
    }
    case MsgType.AGENT_LIST: {
      const truncated = r.bool();
      const agents: AgentEntry[] = [];
      for (let n = r.u16(); n > 0; n--) agents.push({ name: r.str8(), online: r.bool(), capabilities: r.str16() });
      m = { type, truncated, agents };
      break;
    }
    case MsgType.AGENT_REQUEST_IN: {
      const requester = r.str8();
      const handle = r.u64();
      m = { type, requester, handle, body: r.str16() };
      break;
    }
    case MsgType.AGENT_RESPONSE_IN: {
      const agent = r.str8();
      const reqId = r.u64();
      const status = r.u8();
      const final = r.bool();
      m = { type, agent, reqId, status, final, body: r.str16() };
      break;
    }
    default:
      throw new DecodeError(`unknown message type 0x${type.toString(16)}`, true);
  }
  r.done();
  return m;
}

// ---------------------------------------------------------------- frames

/** Builds a frame from a raw payload. */
export function frame(type: number, payload: Uint8Array, seq: number, flags = 0): Uint8Array {
  checkInt(type, 0xff, "type");
  checkInt(seq, 0xffff, "seq");
  checkInt(flags, 0xff, "flags");
  if (payload.length > MAX_PAYLOAD) throw new RangeError(`payload longer than ${MAX_PAYLOAD} bytes`);
  const out = new Uint8Array(HEADER_LEN + payload.length);
  const v = new DataView(out.buffer);
  out[0] = VERSION;
  out[1] = flags;
  v.setUint16(2, seq);
  v.setUint16(4, payload.length);
  out[6] = type;
  out.set(payload, HEADER_LEN);
  return out;
}

/** Encodes a message as a complete frame. */
export function encodeFrame(m: Message, seq: number, flags = 0): Uint8Array {
  return frame(m.type, encodePayload(m), seq, flags);
}

function readHeader(bytes: Uint8Array): Omit<Frame, "payload"> & { length: number } {
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const version = bytes[0] as number;
  if (version !== VERSION) throw new DecodeError(`unsupported protocol version ${version}`);
  return { version, flags: bytes[1] as number, seq: v.getUint16(2), length: v.getUint16(4), type: bytes[6] as number };
}

/** Decodes bytes holding exactly one frame (as a WebSocket message does). */
export function decodeFrame(bytes: Uint8Array): Frame {
  if (bytes.length < HEADER_LEN) throw new DecodeError("frame shorter than its header");
  const h = readHeader(bytes);
  if (bytes.length !== HEADER_LEN + h.length) throw new DecodeError("frame length does not match its header");
  return { version: h.version, flags: h.flags, seq: h.seq, type: h.type, payload: bytes.subarray(HEADER_LEN) };
}

/** Decodes a frame and its payload. */
export function decodeMessage(bytes: Uint8Array): { frame: Frame; message: Message } {
  const f = decodeFrame(bytes);
  return { frame: f, message: decodePayload(f.type, f.payload) };
}

/**
 * Splits a byte stream (TCP) into frames. push() returns every complete
 * frame so far; partial frames are kept until the rest arrives. Throws
 * DecodeError on a bad version, after which the stream cannot be trusted.
 */
export class FrameParser {
  private buf = new Uint8Array(0);

  push(chunk: Uint8Array): Uint8Array[] {
    let data: Uint8Array;
    if (this.buf.length === 0) {
      data = chunk;
    } else {
      data = new Uint8Array(this.buf.length + chunk.length);
      data.set(this.buf);
      data.set(chunk, this.buf.length);
    }
    const out: Uint8Array[] = [];
    let p = 0;
    while (data.length - p >= HEADER_LEN) {
      const h = readHeader(data.subarray(p, p + HEADER_LEN));
      const end = p + HEADER_LEN + h.length;
      if (end > data.length) break;
      out.push(data.slice(p, end));
      p = end;
    }
    this.buf = data.slice(p);
    return out;
  }

  /** Bytes held back waiting for the rest of a frame. */
  get pending(): number {
    return this.buf.length;
  }
}
