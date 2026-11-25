#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# alias-heur.py — 任何函式呼叫裡同一個緩衝區被當成兩個參數（其中一個是 sizeof 的對象或同名參數），人工判斷函式是否容許原地處理。
import re, sys
CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
SKIP = {'if','while','for','switch','return','sizeof','memcmp','strcmp','strncmp','strcasecmp','MAX','MIN','CLAMP','fprintf','printf','snprintf','execl','execlp','memcpy','memmove','strcpy','strcat','strncat','strncpy','sprintf'}
def split_args(s, i):
    depth, args, cur, k, instr = 0, [], '', i, None
    while k < len(s):
        c = s[k]
        if instr:
            cur += c
            if c == '\\': cur += s[k+1]; k += 1
            elif c == instr: instr = None
        elif c in '"\'': instr = c; cur += c
        elif c in '([{': depth += 1; cur += c
        elif c in ')]}':
            if depth == 0: args.append(cur.strip()); return args
            depth -= 1; cur += c
        elif c == ',' and depth == 0: args.append(cur.strip()); cur = ''
        else: cur += c
        k += 1
    return None
for path in sys.argv[1:]:
    s = open(path, encoding='utf-8', errors='replace').read()
    for m in CALL.finditer(s):
        if m.group(1) in SKIP: continue
        args = split_args(s, m.end())
        if not args or len(args) < 3: continue
        plain = [a for a in args if re.fullmatch(r'&?[A-Za-z_]\w*(?:(?:\.|->)\w+)*', a)]
        for a in set(plain):
            if plain.count(a) >= 2 and not a.isupper():
                print('%s:%d: %s(%s)' % (path, s.count('\n', 0, m.start()) + 1, m.group(1), ', '.join(x[:30] for x in args)))
                break
