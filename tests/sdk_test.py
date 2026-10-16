"""End-to-end tests for the Python SDK: python3 tests/sdk_test.py PORT"""
import asyncio
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "sdk", "python"))
import tundra  # noqa: E402

PORT = int(sys.argv[1])
results = []


def check(name, cond):
    results.append(cond)
    print(("ok   - " if cond else "FAIL - ") + "sdk: " + name)


async def first_message(client, timeout=2):
    agen = client.messages()
    try:
        return await asyncio.wait_for(agen.__anext__(), timeout)
    except asyncio.TimeoutError:
        return None


async def main():
    agent = tundra.Agent("127.0.0.1", PORT, "pyagent", capabilities={"skills": ["upper"]})

    @agent.handler
    async def handle(req):
        if req.body == "fail":
            raise ValueError("asked to fail")
        if req.body == "stream":
            yield "part one"
            yield "part two"
            return
        yield req.body.upper()

    await agent.connect()
    alice = tundra.Client("127.0.0.1", PORT, "pyalice")
    bob = tundra.Client("127.0.0.1", PORT, "pybob")
    await alice.connect()
    await bob.connect()

    agents = await alice.agents()
    check("agents lists the agent with parsed capabilities", agents.get("pyagent", {}).get("capabilities") == {"skills": ["upper"]})
    check("request returns the final answer", await alice.request("pyagent", "shout") == "SHOUT")
    parts = [p async for p in alice.request_stream("pyagent", "stream")]
    check("request_stream yields partial and final answers", parts == ["part one", "part two"])
    try:
        await alice.request("pyagent", "fail")
        check("handler exceptions become an error status", False)
    except tundra.AgentError as e:
        check("handler exceptions become an error status", e.status == 1 and "asked to fail" in e.body)
    try:
        await alice.request("nobody-here", "x")
        check("unknown agent raises TundraError", False)
    except tundra.TundraError as e:
        check("unknown agent raises TundraError", e.code == 6)

    mid = await bob.send("#lobby", "hello from bob")
    msg = await first_message(alice)
    check("send returns an id and the message arrives", msg is not None and msg.id == mid and msg.text == "hello from bob")
    await bob.send("pyalice", "a direct one")
    msg = await first_message(alice)
    check("direct messages are marked direct", msg is not None and msg.direct and msg.sender == "pybob")

    await alice.join("#py")
    await bob.join("#py")
    await asyncio.sleep(0.1)
    await bob.send("#py", "for history")
    await first_message(alice)
    hist = await alice.history("#py")
    check("history returns room messages", [m.text for m in hist] == ["for history"])
    rooms = await alice.rooms()
    check("rooms lists joined rooms", rooms.get("#py") == 2)

    # drop alice's connection; bob keeps talking; alice reconnects and catches up
    alice._writer.transport.abort()
    await asyncio.sleep(0.1)
    await bob.send("#py", "while you were gone")
    msg = await first_message(alice, timeout=5)
    check("automatic reconnect catches up on missed messages", msg is not None and msg.text == "while you were gone")
    check("session id survives the reconnect", alice.session_id is not None)

    # a refused connect does not leave a reconnect loop behind
    dup = tundra.Client("127.0.0.1", PORT, "pybob")
    try:
        await dup.connect()
        check("taken name is refused", False)
    except tundra.TundraError as e:
        check("taken name is refused", e.code == 1)
    check("refused client leaves no background task", dup._task is None and dup._writer is None)

    for c in (alice, bob, agent):
        await c.close()


asyncio.run(main())
sys.exit(0 if all(results) else 1)
