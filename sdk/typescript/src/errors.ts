import { STATUS_NAMES } from "./protocol.js";

/** An ERROR frame from the server. */
export class TundraError extends Error {
  readonly code: number;
  readonly text: string;
  constructor(code: number, text: string) {
    super(`${code}: ${text}`);
    this.name = "TundraError";
    this.code = code;
    this.text = text;
  }
}

/**
 * An agent answered with a non-ok status, or the server closed the request.
 * Agent handlers may also throw one to answer with a specific status.
 */
export class AgentError extends Error {
  readonly agent: string;
  readonly status: number;
  readonly body: string;
  constructor(agent: string, status: number, body: string) {
    super(`${agent} answered ${STATUS_NAMES[status] ?? String(status)}: ${body}`);
    this.name = "AgentError";
    this.agent = agent;
    this.status = status;
    this.body = body;
  }
}

/** The connection is not open, or was lost before an answer arrived. */
export class ConnectionError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "ConnectionError";
  }
}

/** No answer arrived in time. */
export class RequestTimeoutError extends Error {
  constructor(message = "timed out") {
    super(message);
    this.name = "RequestTimeoutError";
  }
}

/** Bytes that do not form a valid Tundra frame or payload. */
export class DecodeError extends Error {
  /** True when the message type is unknown (the payload was not inspected). */
  readonly unknownType: boolean;
  constructor(message: string, unknownType = false) {
    super(message);
    this.name = "DecodeError";
    this.unknownType = unknownType;
  }
}
