"""An agent that echoes requests, streaming one partial answer first.

    python3 echo_agent.py [host] [port]
"""
import asyncio
import sys

sys.path.insert(0, __file__.rsplit("/examples/", 1)[0])
import tundra  # noqa: E402

host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
port = int(sys.argv[2]) if len(sys.argv) > 2 else 4242
agent = tundra.Agent(host, port, "echo", capabilities={"skills": ["echo"], "streams": True})


@agent.handler
async def handle(req):
    yield "thinking about: " + req.body
    await asyncio.sleep(0.2)
    yield "echo: " + req.body


asyncio.run(agent.run_forever())
