#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""檢查翻譯檔：列出原始碼中還沒翻譯的句子，以及已經用不到的翻譯。

    python3 libpomelo/i18n/check.py libpomelo/i18n/en.tsv

新增語言：複製 en.tsv 成例如 ja.tsv，把每行 Tab 後面的譯文換掉即可
（檔名用語言代碼：ja、ko、zh_CN…）。
"""
import glob, os, re, sys

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
lit = r'"((?:[^"\\]|\\.)*)"'
pat = re.compile(r'\b(?:N?_|pm_tr)\(\s*((?:' + lit + r'\s*)+)\)')

def unc(s):
    return s.encode('utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')

ids = []
for f in sorted(glob.glob(root + '/libpomelo/*.c') + glob.glob(root + '/pomelo-*/*.[ch]') + glob.glob(root + '/pomelo-doctor/intrusion/*.[ch]')):
    for m in pat.finditer(open(f, encoding='utf-8').read()):
        mid = unc(''.join(re.findall(lit, m.group(1))))
        if mid not in ids:
            ids.append(mid)

def esc(s):
    return s.replace('\\', '\\\\').replace('\t', '\\t').replace('\n', '\\n')

have = {}
for line in open(sys.argv[1], encoding='utf-8'):
    line = line.rstrip('\n')
    if line.startswith('#') or '\t' not in line:
        continue
    k, v = line.split('\t', 1)
    have[k] = v
missing = [m for m in ids if esc(m) not in have]
# printf / strftime 的 %… 符號要跟原文一樣多（順序可以不同，例如日期）
fmt = re.compile(r'%[-+#0-9.]*(?:ld|lld|lu|zu|[a-zA-Z])')
tm = re.compile(r'%-?[YHMAaBbpI]|%-[md]')   # strftime 格式：各語言的順序與欄位本來就不同，只要求還有 % 符號
bad = [k for k, v in have.items()
       if (('%' not in v) if tm.search(k) else sorted(fmt.findall(k)) != sorted(fmt.findall(v)))]
unused = [k for k in have if k not in {esc(m) for m in ids}]
print(f'共 {len(ids)} 句，已翻譯 {len(ids) - len(missing)} 句')
for m in missing:
    print('未翻譯：' + esc(m))
for k in unused:
    print('用不到：' + k)
for k in bad:
    print('格式符號不符：' + k + ' → ' + have[k])
sys.exit(1 if missing or bad else 0)
