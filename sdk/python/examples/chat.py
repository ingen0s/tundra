"""Minimal terminal chat: prints the lobby and sends each line you type.

    python3 chat.py username [host] [port]
"""
import asyncio
import sys

sys.path.insert(0, __file__.rsplit("/examples/", 1)[0])
import tundra  # noqa: E402


async def main():
    name = sys.argv[1]
    host = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 4242
    client = tundra.Client(host, port, name)
    await client.connect()
    for msg in await client.history("#lobby", limit=20):
        print("%s: %s" % (msg.sender, msg.text))

    async def show():
        async for msg in client.messages():
            where = "(dm) " if msg.direct else ""
            print("%s%s: %s" % (where, msg.sender, msg.text))

    asyncio.ensure_future(show())
    loop = asyncio.get_event_loop()
    while True:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if not line:
            break
        if line.strip():
            await client.send("#lobby", line.rstrip("\n"))
    await client.close()


asyncio.run(main())
