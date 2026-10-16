#!/usr/bin/env python3
"""Turns a file into a C header with a byte array: embed.py INPUT OUTPUT NAME."""
import sys

src, dst, name = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, "rb").read()
with open(dst, "w") as out:
    out.write("/* generated from %s by tools/embed.py; do not edit */\n" % src)
    out.write("static const unsigned char %s[] = {\n" % name)
    for i in range(0, len(data), 16):
        out.write("    " + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",\n")
    out.write("};\nstatic const size_t %s_len = %d;\n" % (name, len(data)))
