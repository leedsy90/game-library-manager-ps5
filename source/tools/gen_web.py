#!/usr/bin/env python3
"""Embed web/index.html into src/web_index.c as a C string."""
import sys
src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
out = ['/* generated from web/index.html - do not edit */', '#include "common.h"', 'const char g_index_html[] =']
line = '"'
for b in data:
    c = chr(b)
    if c == '\\': s = '\\\\'
    elif c == '"': s = '\\"'
    elif c == '\n': s = '\\n'
    elif c == '\t': s = '\\t'
    elif c == '?': s = '\\?'   # avoid trigraphs
    elif 32 <= b < 127: s = c
    else: s = '\\%03o' % b
    line += s
    if c == '\n' or len(line) > 100:
        out.append(line + '"')
        line = '"'
out.append(line + '";')
open(dst, "w").write("\n".join(out) + "\n")
