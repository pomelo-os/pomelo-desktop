#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# overlap-heur.py — 啟發式找「來源與目的是同一塊緩衝區」的字串呼叫（snprintf/sprintf/strcpy/strcat/strncpy/strncat/stpcpy/memcpy）。
# 編譯器的 -Wrestrict 只抓得到看得出同一個物件的；這支補「目的是結構成員／指標運算」的情形。結果要人工判斷。
# 用法：python3 overlap-heur.py 檔案…
import re, sys
FN = re.compile(r'\b(snprintf|sprintf|strcpy|strcat|strncpy|strncat|stpcpy|memcpy)\s*\(')
def split_args(s, i):
    depth, args, cur, k = 0, [], '', i
    instr = None
    while k < len(s):
        c = s[k]
        if instr:
            cur += c
            if c == '\\': cur += s[k+1]; k += 1
            elif c == instr: instr = None
        elif c in '"\'': instr = c; cur += c
        elif c in '([{': depth += 1; cur += c
        elif c in ')]}':
            if depth == 0: args.append(cur.strip()); return args, k
            depth -= 1; cur += c
        elif c == ',' and depth == 0: args.append(cur.strip()); cur = ''
        else: cur += c
        k += 1
    return None, k
def strip_safe(a):
    a = re.sub(r'sizeof\s*\(?[^,)]*\)?', ' ', a)           # sizeof dest
    a = re.sub(r'\bstrlen\s*\([^()]*\)', ' ', a)             # strlen(dest)
    a = re.sub(r'"(\\.|[^"\\])*"', '""', a)
    return a
for path in sys.argv[1:]:
    try: s = open(path, encoding='utf-8', errors='replace').read()
    except Exception: continue
    for m in FN.finditer(s):
        args, end = split_args(s, m.end())
        if not args or len(args) < 2: continue
        dest = args[0]
        if dest.startswith('(') and ')' in dest: dest = dest[dest.index(')')+1:].strip()
        base = re.match(r'[&*]?\s*([A-Za-z_]\w*(?:(?:\.|->)\w+)*)', dest)
        if not base: continue
        d = base.group(1)
        dpat = re.compile(r'(?<![\w.>])' + re.escape(d) + r'(?![\w(\[]|\s*\()')
        hits = [a for a in args[1:] if dpat.search(strip_safe(a))]
        if hits:
            line = s.count('\n', 0, m.start()) + 1
            print('%s:%d: %s(%s, ...) 來源含 %s' % (path, line, m.group(1), dest, ' | '.join(h[:60] for h in hits)))
