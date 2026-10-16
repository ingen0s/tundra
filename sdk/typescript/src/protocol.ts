/** Wire constants for Tundra protocol version 1 (see docs/PROTOCOL.md). */

export const VERSION = 0x01;
export const HEADER_LEN = 7;
export const MAX_PAYLOAD = 0xffff;
export const MAX_FRAME = HEADER_LEN + MAX_PAYLOAD;
export const UUID_LEN = 16;
export const MAX_USERNAME = 32;
export const MAX_ROOM = 32;
export const MAX_HISTORY = 100;
/** Largest chat text in bytes; fits every frame that carries it. */
export const MAX_TEXT = MAX_PAYLOAD - 35 - 2 - 85;
export const MAX_CAPABILITIES = 8192;

/** Message types. 0x01-0x0F and 0x20-0x2F are client to server, the rest server to client. */
export enum MsgType {
  REGISTER = 0x01,
  MESSAGE = 0x02,
  AGENT_COMMAND = 0x03,
  PONG = 0x04,
  HEALTH_CHECK = 0x05,
  JOIN = 0x06,
  LEAVE = 0x07,
  SEND = 0x08,
  LIST_ROOMS = 0x09,
  ACK = 0x0a,
  HISTORY = 0x0b,
  WHO = 0x0c,
  AGENT_ADVERTISE = 0x20,
  AGENT_REQUEST = 0x21,
  AGENT_RESPONSE = 0x22,
  LIST_AGENTS = 0x23,

  SYSTEM = 0x10,
  BROADCAST_MESSAGE = 0x11,
  BROADCAST_AGENT = 0x12,
  PING = 0x14,
  ERROR = 0x15,
  HEALTH_RESULT = 0x16,
  CHAT = 0x17,
  SENT = 0x18,
  ROOM_LIST = 0x19,
  ROOM_EVENT = 0x1a,
  HISTORY_RESULT = 0x1b,
  WHO_RESULT = 0x1c,
  AGENT_LIST = 0x30,
  AGENT_REQUEST_IN = 0x31,
  AGENT_RESPONSE_IN = 0x32,
}

/** REGISTER extension tags. */
export enum RegisterTag {
  RESUME_FROM = 0x01,
  TOKEN = 0x02,
}

/** CHAT / SEND kinds. */
export enum ChatKind {
  ROOM = 0,
  DIRECT = 1,
}

/** ROOM_EVENT events. */
export enum RoomEventKind {
  JOINED = 1,
  LEFT = 2,
}

/** ERROR codes. */
export enum ErrorCode {
  USERNAME_TAKEN = 0x0001,
  INVALID_FORMAT = 0x0002,
  SESSION_EXPIRED = 0x0003,
  SERVER_ERROR = 0x0004,
  NOT_REGISTERED = 0x0005,
  NO_SUCH_TARGET = 0x0006,
  NOT_A_MEMBER = 0x0007,
  INVALID_NAME = 0x0008,
  UNAUTHORIZED = 0x0009,
  NOT_AN_AGENT = 0x000a,
  BAD_REQUEST = 0x000b,
}

/** Agent response statuses. */
export enum AgentStatus {
  OK = 0,
  ERROR = 1,
  REJECTED = 2,
  TIMEOUT = 3,
  UNAVAILABLE = 4,
}

export const STATUS_NAMES: Readonly<Record<number, string>> = {
  0: "ok",
  1: "error",
  2: "rejected",
  3: "timeout",
  4: "unavailable",
};

/** Errors that retrying cannot fix: name taken, invalid name, unauthorized. */
export const FATAL_ERRORS: ReadonlySet<number> = new Set([
  ErrorCode.USERNAME_TAKEN,
  ErrorCode.INVALID_NAME,
  ErrorCode.UNAUTHORIZED,
]);
