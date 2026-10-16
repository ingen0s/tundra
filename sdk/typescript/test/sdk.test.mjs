// End-to-end tests for the TypeScript SDK against a live server.
//
//   TUNDRA_PORT=4242 TUNDRA_WS_PORT=8080 node --test test/sdk.test.mjs
//
// The server must run with `-w TUNDRA_WS_PORT` for the WebSocket tests.
// TUNDRA_EXPIRY_PORT, if set, names a second server started with `-g 0`
// (sessions expire at once) for the session-expiry tests. TUNDRA_TLS_PORT and
// TUNDRA_WSS_PORT name a server started with `-C cert -K key -w`, whose CA is
// in TUNDRA_TLS_CA (and NODE_EXTRA_CA_CERTS, so wss:// trusts it);
// TUNDRA_TLS_OTHER_CA is an unrelated CA for the untrusted-certificate test.
// tests/ts_sdk.sh at the repository root builds and starts the servers for you.
import { after, before, describe, it } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import net from "node:net";
import {
  Agent,
  AgentError,
  Client,
  ErrorCode,
  MsgType,
  TundraError,
  decodeMessage,
  encodeFrame,
  FrameParser,
} from "../dist/index.js";

const HOST = "127.0.0.1";
const PORT = Number(process.env.TUNDRA_PORT ?? 4242);
const WS_PORT = Number(process.env.TUNDRA_WS_PORT ?? 0);
const WS_URL = `ws://${HOST}:${WS_PORT}/ws`;
const EXPIRY_PORT = Number(process.env.TUNDRA_EXPIRY_PORT ?? 0);
const TLS_PORT = Number(process.env.TUNDRA_TLS_PORT ?? 0);
const WSS_PORT = Number(process.env.TUNDRA_WSS_PORT ?? 0);
const TLS_CA = process.env.TUNDRA_TLS_CA;
const TLS_OTHER_CA = process.env.TUNDRA_TLS_OTHER_CA;
// unique names so repeated runs against one server do not collide
const tag = Math.random().toString(36).slice(2, 7);
const name = (n) => `ts${n}-${tag}`;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

describe("codec", () => {
  it("round-trips every message type through a frame", () => {
    const id = new Uint8Array(16).fill(7);
    const chat = { id: 2n ** 63n + 5n, ts: 1700000000000n, kind: 0, target: "#dev", sender: "bob", text: "héllo" };
    const msgs = [
      { type: MsgType.REGISTER, id, username: "alice", extensions: [{ tag: 1, value: new Uint8Array(8) }, { tag: 2, value: new Uint8Array([65]) }] },
      { type: MsgType.MESSAGE, text: "hi" },
      { type: MsgType.AGENT_COMMAND, agent: "a", command: "c" },
      { type: MsgType.PONG, data: new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]) },
      { type: MsgType.HEALTH_CHECK },
      { type: MsgType.JOIN, room: "#x" },
      { type: MsgType.LEAVE, room: "#x" },
      { type: MsgType.SEND, kind: 1, target: "bob", text: "yo" },
      { type: MsgType.LIST_ROOMS },
      { type: MsgType.ACK, id: 42n },
      { type: MsgType.HISTORY, room: "#x", beforeId: 0n, limit: 50 },
      { type: MsgType.WHO, room: "#x" },
      { type: MsgType.AGENT_ADVERTISE, capabilities: "{}" },
      { type: MsgType.AGENT_REQUEST, agent: "a", reqId: 1n, body: "b" },
      { type: MsgType.AGENT_RESPONSE, requester: "r", handle: 9n, status: 0, final: true, body: "ok" },
      { type: MsgType.LIST_AGENTS },
      { type: MsgType.SYSTEM, id, text: "welcome" },
      { type: MsgType.BROADCAST_AGENT, sender: "s", ts: 1n, text: "a c" },
      { type: MsgType.PING, ts: 123n },
      { type: MsgType.ERROR, code: 6, text: "nope" },
      { type: MsgType.HEALTH_RESULT, ts: 1n, status: 0, latencyMs: 2, uptimeMs: 3, sent: 4, received: 5 },
      { type: MsgType.CHAT, ...chat },
      { type: MsgType.SENT, id: 3n },
      { type: MsgType.ROOM_LIST, rooms: [{ room: "#lobby", members: 3 }] },
      { type: MsgType.ROOM_EVENT, event: 1, room: "#x", user: "u" },
      { type: MsgType.HISTORY_RESULT, room: "#dev", messages: [chat, { ...chat, id: 1n }] },
      { type: MsgType.WHO_RESULT, room: "#x", members: [{ user: "u", online: true }] },
      { type: MsgType.AGENT_LIST, truncated: true, agents: [{ name: "a", online: false, capabilities: "{}" }] },
      { type: MsgType.AGENT_REQUEST_IN, requester: "r", handle: 77n, body: "q" },
      { type: MsgType.AGENT_RESPONSE_IN, agent: "a", reqId: 5n, status: 3, final: false, body: "" },
    ];
    for (const m of msgs) {
      const bytes = encodeFrame(m, 513);
      const { frame, message } = decodeMessage(bytes);
      assert.equal(frame.seq, 513);
      assert.deepEqual(message, m, MsgType[m.type]);
    }
  });

  it("rejects truncated, trailing and bad-version frames", () => {
    const good = encodeFrame({ type: MsgType.ERROR, code: 1, text: "x" }, 0);
    assert.throws(() => decodeMessage(good.subarray(0, good.length - 1)));
    const trailing = encodeFrame({ type: MsgType.SENT, id: 1n }, 0);
    const longer = new Uint8Array(trailing.length + 1);
    longer.set(trailing);
    longer[5] += 1; // length field covers the extra byte, payload is now too long
    assert.throws(() => decodeMessage(longer), /trailing/);
    const bad = good.slice();
    bad[0] = 2;
    assert.throws(() => decodeMessage(bad), /version/);
    assert.throws(() => encodeFrame({ type: MsgType.JOIN, room: "x".repeat(256) }, 0), RangeError);
  });

  it("rejects an AGENT_LIST truncated flag other than 0 or 1", () => {
    const f = encodeFrame({ type: MsgType.AGENT_LIST, truncated: false, agents: [] }, 0);
    f[7] = 2;
    assert.throws(() => decodeMessage(f), /truncated/);
    f[7] = 1;
    assert.equal(decodeMessage(f).message.truncated, true);
  });

  it("splits a byte stream into frames", () => {
    const a = encodeFrame({ type: MsgType.PING, ts: 1n }, 1);
    const b = encodeFrame({ type: MsgType.SENT, id: 2n }, 2);
    const all = new Uint8Array([...a, ...b]);
    const p = new FrameParser();
    const out = [];
    for (const byte of all) out.push(...p.push(Uint8Array.of(byte)));
    assert.equal(out.length, 2);
    assert.deepEqual(out[1], b);
    assert.equal(p.pending, 0);
  });
});

describe("TCP client and agent", () => {
  let agent, alice, bob;
  const agentName = name("agent");

  before(async () => {
    agent = new Agent({ host: HOST, port: PORT, username: agentName, capabilities: { skills: ["upper"] } });
    agent.handler(async function* (req) {
      if (req.body === "fail") throw new Error("asked to fail");
      if (req.body === "reject") throw new AgentError(agentName, 2, "not today");
      if (req.body === "stream") {
        yield "part one";
        yield "part two";
        return;
      }
      yield req.body.toUpperCase();
    });
    await agent.connect();
    alice = await Client.connect({ host: HOST, port: PORT, username: name("alice") });
    bob = await Client.connect({ host: HOST, port: PORT, username: name("bob") });
  });

  after(async () => {
    for (const c of [alice, bob, agent]) await c?.close();
  });

  it("registers with a session id", () => {
    assert.match(alice.sessionId, /^[0-9a-f]{8}-[0-9a-f]{4}-/);
    assert.equal(alice.state, "connected");
  });

  it("lists agents with parsed capabilities", async () => {
    const agents = await alice.agents();
    assert.deepEqual(agents[agentName], { online: true, capabilities: { skills: ["upper"] } });
    assert.equal(alice.agentsTruncated, false);
  });

  it("request returns the final answer", async () => {
    assert.equal(await alice.request(agentName, "shout"), "SHOUT");
  });

  it("requestStream yields partial and final answers", async () => {
    const parts = [];
    for await (const p of alice.requestStream(agentName, "stream")) parts.push(p);
    assert.deepEqual(parts, ["part one", "part two"]);
  });

  it("handler exceptions become an error status", async () => {
    await assert.rejects(alice.request(agentName, "fail"), (e) => e instanceof AgentError && e.status === 1 && e.body.includes("asked to fail"));
    await assert.rejects(alice.request(agentName, "reject"), (e) => e instanceof AgentError && e.status === 2 && e.body === "not today");
  });

  it("a plain-function handler that returns works too", async () => {
    const a2name = name("plain");
    const a2 = await Agent.connect({ host: HOST, port: PORT, username: a2name, handler: (req) => `got ${req.body}` });
    try {
      assert.equal(await alice.request(a2name, "x"), "got x");
    } finally {
      await a2.close();
    }
  });

  it("unknown agent raises TundraError", async () => {
    await assert.rejects(alice.request(name("nobody"), "x"), (e) => e instanceof TundraError && e.code === ErrorCode.NO_SUCH_TARGET);
    await assert.rejects(alice.request(bob.username, "x"), (e) => e instanceof TundraError && e.code === ErrorCode.NOT_AN_AGENT);
  });

  it("send returns an id and the message arrives", async () => {
    const id = await bob.send("#lobby", "hello from bob");
    assert.equal(typeof id, "bigint");
    const msg = await alice.nextMessage(2000);
    assert.ok(msg);
    assert.equal(msg.id, id);
    assert.equal(msg.text, "hello from bob");
    assert.equal(msg.room, "#lobby");
    assert.equal(msg.direct, false);
  });

  it("direct messages are marked direct", async () => {
    await bob.send(alice.username, "a direct one");
    const msg = await alice.nextMessage(2000);
    assert.ok(msg);
    assert.equal(msg.direct, true);
    assert.equal(msg.room, null);
    assert.equal(msg.sender, bob.username);
  });

  it("join, history, rooms and who", async () => {
    const room = `#ts-${tag}`;
    await alice.join(room);
    await bob.join(room);
    assert.ok(alice.joinedRooms.has(room));
    await bob.send(room, "for history");
    const live = await alice.nextMessage(2000);
    assert.equal(live?.text, "for history");
    const hist = await alice.history(room);
    assert.deepEqual(hist.map((m) => m.text), ["for history"]);
    const rooms = await alice.rooms();
    assert.equal(rooms[room], 2);
    const who = await alice.who(room);
    assert.deepEqual(who.map((w) => w.user).sort(), [alice.username, bob.username].sort());
    await assert.rejects(alice.join(room), (e) => e instanceof TundraError && e.code === ErrorCode.INVALID_FORMAT);
    await assert.rejects(alice.join("#Bad Name"), (e) => e instanceof TundraError && e.code === ErrorCode.INVALID_NAME);
  });

  it("health check answers", async () => {
    const h = await alice.health();
    assert.equal(typeof h.uptimeMs, "number");
  });

  it("messages() iterates and stops on break", async () => {
    await bob.send("#lobby", "iter one");
    await bob.send("#lobby", "iter two");
    const got = [];
    for await (const m of alice.messages()) {
      got.push(m.text);
      if (got.length === 2) break;
    }
    assert.deepEqual(got, ["iter one", "iter two"]);
    // break cancelled the iterator's wait, so the next message goes to the next reader
    await bob.send("#lobby", "after break");
    assert.equal((await alice.nextMessage(2000))?.text, "after break");
  });

  it("reconnects after a dropped socket and catches up", async () => {
    const room = `#ts-${tag}`;
    const session = alice.sessionId;
    const events = [];
    const off = alice.on("connected", (e) => events.push(e));
    alice.transport.close(); // simulate the connection dropping
    assert.equal(alice.state, "reconnecting");
    await sleep(50);
    await bob.send(room, "while you were gone");
    const msg = await alice.nextMessage(5000);
    off();
    assert.equal(msg?.text, "while you were gone");
    assert.equal(alice.sessionId, session, "session id survives the reconnect");
    assert.deepEqual(events.map((e) => e.resumed), [true]);
    // nothing is delivered twice by the replay
    assert.equal(await alice.nextMessage(300), null);
  });

  it("calls made while disconnected fail fast", async () => {
    const c = new Client({ host: HOST, port: PORT, username: name("idle") });
    await assert.rejects(c.send("#lobby", "x"), /not connected/);
  });

  it("a failed connect ends messages(), nextMessage() and ready()", async () => {
    const probe = net.createServer();
    await new Promise((r) => probe.listen(0, HOST, r));
    const deadPort = probe.address().port;
    await new Promise((r) => probe.close(r)); // nothing listens there now
    const c = new Client({ host: HOST, port: deadPort, username: name("dead") });
    const it = c.messages();
    const pendingNext = it.next();
    const closed = c.next("closed");
    const connecting = c.connect();
    const ready = c.ready();
    await assert.rejects(connecting, /ECONNREFUSED/);
    await assert.rejects(pendingNext, /ECONNREFUSED/);
    await assert.rejects(ready, /ECONNREFUSED/);
    await assert.rejects(c.nextMessage(100), /ECONNREFUSED/);
    assert.match(String((await closed).error), /ECONNREFUSED/);
    assert.match(String(c.error), /ECONNREFUSED/);
    await c.close(); // still fine to call
  });

  it("close() always ends messages()", async () => {
    const c = await Client.connect({ host: HOST, port: PORT, username: name("closer") });
    const it = c.messages();
    const pendingNext = it.next();
    await c.close();
    assert.deepEqual(await pendingNext, { done: true, value: undefined });
    await c.close();
    assert.equal(await c.nextMessage(100), null);
  });

  it("an agent's answer survives a dropped socket while the handler runs", async () => {
    const slowName = name("slow");
    const slow = await Agent.connect({
      host: HOST,
      port: PORT,
      username: slowName,
      reconnectDelayMs: 600, // still reconnecting when the handler finishes
      handler: async (req) => {
        await sleep(300);
        return `done: ${req.body}`;
      },
    });
    const errors = [];
    slow.on("error", (e) => errors.push(e));
    try {
      const answer = alice.request(slowName, "q", { timeoutMs: 5000 });
      await sleep(100);
      const resumed = slow.next("connected");
      slow.transport.close();
      assert.equal(slow.state, "reconnecting");
      assert.equal(await answer, "done: q");
      assert.equal((await resumed).resumed, true);
      assert.deepEqual(errors, []);
    } finally {
      await slow.close();
    }
  });

  it("refused registration leaves no background reconnect", async () => {
    const dup = new Client({ host: HOST, port: PORT, username: bob.username });
    const retries = [];
    dup.on("reconnecting", (e) => retries.push(e));
    await assert.rejects(dup.connect(), (e) => e instanceof TundraError && e.code === ErrorCode.USERNAME_TAKEN);
    assert.equal(dup.state, "closed");
    assert.equal(dup.transport, null);
    await sleep(500);
    assert.deepEqual(retries, []);
    assert.equal(dup.transport, null);
    await assert.rejects(Client.connect({ host: HOST, port: PORT, username: "has space" }), (e) => e.code === ErrorCode.INVALID_NAME);
  });

  it("gives up when a reconnect is refused with a fatal error", async () => {
    const victim = await Client.connect({ host: HOST, port: PORT, username: name("victim"), reconnectDelayMs: 50 });
    // Forget the session so the reconnect registers fresh: the server still holds
    // the name for the detached session, so the fresh registration is refused.
    victim.sessionBytes = null;
    const closed = victim.next("closed");
    victim.transport.close();
    const e = await closed;
    assert.equal(e.error?.code, ErrorCode.USERNAME_TAKEN);
    assert.equal(victim.state, "closed");
    assert.equal(victim.error?.code, ErrorCode.USERNAME_TAKEN);
    await assert.rejects(victim.nextMessage(100), (err) => err.code === ErrorCode.USERNAME_TAKEN);
    await sleep(300);
    assert.equal(victim.transport, null, "no reconnect after giving up");
  });
});

describe("WebSocket transport", { skip: WS_PORT ? false : "set TUNDRA_WS_PORT" }, () => {
  let agent, carol, dave;
  const agentName = name("wsagent");

  before(async () => {
    agent = await Agent.connect({
      url: WS_URL,
      username: agentName,
      capabilities: "plain text caps",
      handler: function* (req) {
        yield "thinking";
        yield `echo: ${req.body}`;
      },
    });
    carol = await Client.connect({ url: WS_URL, username: name("carol") });
    dave = await Client.connect({ host: HOST, port: PORT, username: name("dave") }); // TCP peer
  });

  after(async () => {
    for (const c of [carol, dave, agent]) await c?.close();
  });

  it("registers over WebSocket", () => {
    assert.equal(carol.state, "connected");
    assert.ok(carol.sessionId);
  });

  it("sends and receives across transports", async () => {
    const id = await dave.send("#lobby", "tcp to ws");
    const msg = await carol.nextMessage(2000);
    assert.equal(msg?.id, id);
    assert.equal(msg?.text, "tcp to ws");
    await carol.send(dave.username, "ws dm");
    const dm = await dave.nextMessage(2000);
    assert.equal(dm?.text, "ws dm");
    assert.equal(dm?.direct, true);
  });

  it("history and rooms over WebSocket", async () => {
    const room = `#ws-${tag}`;
    await carol.join(room);
    await carol.send(room, "kept");
    assert.deepEqual((await carol.history(room)).map((m) => m.text), ["kept"]);
    assert.equal((await carol.rooms())[room], 1);
  });

  it("agents over WebSocket, with string capabilities", async () => {
    const agents = await carol.agents();
    assert.deepEqual(agents[agentName], { online: true, capabilities: "plain text caps" });
    const parts = [];
    for await (const p of carol.requestStream(agentName, "hi")) parts.push(p);
    assert.deepEqual(parts, ["thinking", "echo: hi"]);
    assert.equal(await dave.request(agentName, "tcp"), "echo: tcp");
  });

  it("reconnects over WebSocket and catches up", async () => {
    const session = carol.sessionId;
    carol.transport.close();
    await sleep(50);
    await dave.send(carol.username, "missed you");
    const msg = await carol.nextMessage(5000);
    assert.equal(msg?.text, "missed you");
    assert.equal(carol.sessionId, session);
  });

  it("refused registration over WebSocket", async () => {
    await assert.rejects(Client.connect({ url: WS_URL, username: carol.username }), (e) => e.code === ErrorCode.USERNAME_TAKEN);
  });
});

describe("session expiry", { skip: EXPIRY_PORT ? false : "set TUNDRA_EXPIRY_PORT (a server with -g 0)" }, () => {
  it("connect() after close() starts a fresh session when the old one expired", async () => {
    const c = await Client.connect({ host: HOST, port: EXPIRY_PORT, username: name("gwen") });
    const first = c.sessionId;
    await c.close();
    await sleep(1500); // the server releases the detached session
    await c.connect();
    assert.equal(c.state, "connected");
    assert.notEqual(c.sessionId, first);
    await c.close();
  });

  it("drops queued agent answers when the session comes back fresh", async () => {
    const agentName = name("expagent");
    const ag = await Agent.connect({
      host: HOST,
      port: EXPIRY_PORT,
      username: agentName,
      reconnectDelayMs: 1500, // the server releases the session meanwhile
      handler: async (req) => {
        await sleep(300);
        return `late: ${req.body}`;
      },
    });
    const asker = await Client.connect({ host: HOST, port: EXPIRY_PORT, username: name("asker") });
    const errors = [];
    ag.on("error", (e) => errors.push(e));
    try {
      const answer = asker.request(agentName, "q", { timeoutMs: 5000 });
      await sleep(100);
      const back = ag.next("connected");
      ag.transport.close();
      // the server closes the request when the session ends
      await assert.rejects(answer, (e) => e instanceof AgentError && e.status === 4);
      assert.equal((await back).resumed, false);
      await sleep(200);
      assert.deepEqual(errors, [], "the stale answer was not sent to the new session");
      assert.equal(await asker.request(agentName, "again"), "late: again");
    } finally {
      await ag.close();
      await asker.close();
    }
  });

  it("registers a fresh session, rejoins rooms and keeps working", async () => {
    const room = `#exp-${tag}`;
    const erin = await Client.connect({ host: HOST, port: EXPIRY_PORT, username: name("erin"), reconnectDelayMs: 1500 });
    const finn = await Client.connect({ host: HOST, port: EXPIRY_PORT, username: name("finn") });
    try {
      await erin.join(room);
      await finn.join(room);
      await finn.send(room, "before");
      assert.equal((await erin.nextMessage(2000))?.text, "before");
      const oldSession = erin.sessionId;
      const connected = erin.next("connected");
      erin.transport.close(); // the server releases the session before the reconnect
      const ev = await connected;
      assert.equal(ev.resumed, false);
      assert.notEqual(erin.sessionId, oldSession);
      // the client rejoins the room it was in; wait for the confirmation
      for (let i = 0; i < 40 && !erin.joinedRooms.has(room); i++) await sleep(50);
      assert.ok(erin.joinedRooms.has(room), "rejoined the room");
      await finn.send(room, "after");
      assert.equal((await erin.nextMessage(2000))?.text, "after");
    } finally {
      await erin.close();
      await finn.close();
    }
  });
});

describe("built-in server TLS", { skip: TLS_PORT && TLS_CA ? false : "set TUNDRA_TLS_PORT and TUNDRA_TLS_CA (a server with -C/-K)" }, () => {
  const ca = TLS_CA ? readFileSync(TLS_CA) : null;

  it("TCP over TLS with tls: { ca }", async () => {
    const c = await Client.connect({ host: HOST, port: TLS_PORT, username: name("tls"), tls: { ca } });
    try {
      assert.equal(c.state, "connected");
      await c.join(`#tls-${tag}`);
      await c.send(`#tls-${tag}`, "secure");
      assert.deepEqual((await c.history(`#tls-${tag}`)).map((m) => m.text), ["secure"]);
    } finally {
      await c.close();
    }
  });

  it("wss:// to the same server, talking to a TLS TCP client", { skip: WSS_PORT ? false : "set TUNDRA_WSS_PORT" }, async () => {
    const w = await Client.connect({ url: `wss://localhost:${WSS_PORT}/ws`, username: name("wss") });
    const t = await Client.connect({ host: "localhost", port: TLS_PORT, username: name("tls2"), tls: { ca } });
    try {
      await t.send(w.username, "over tls");
      const m = await w.nextMessage(2000);
      assert.equal(m?.text, "over tls");
      assert.equal(m?.direct, true);
    } finally {
      await w.close();
      await t.close();
    }
  });

  it("refuses an untrusted certificate without retrying", { skip: TLS_OTHER_CA ? false : "set TUNDRA_TLS_OTHER_CA" }, async () => {
    const c = new Client({ host: HOST, port: TLS_PORT, username: name("tlsbad"), tls: { ca: readFileSync(TLS_OTHER_CA) } });
    const retries = [];
    c.on("reconnecting", (e) => retries.push(e));
    await assert.rejects(c.connect(), /certificate|self.signed|verify/i);
    assert.equal(c.state, "closed");
    await sleep(300);
    assert.deepEqual(retries, []);
    assert.equal(c.transport, null);
  });
});
