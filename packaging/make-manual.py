#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""從 docs/使用手冊.md.in 產生某個發行版的使用手冊。

    python3 packaging/make-manual.py ubuntu|debian|fedora|rocky 版本 > 使用手冊.md

範本裡用 <!--if 條件--> … <!--endif--> 包住只給某些發行版看的段落（條件：ubuntu、debian、deb、fedora、rocky、rpm），
{{名稱}} 之類的會換成這個發行版的文字。
"""
import datetime, os, re, sys

which, version = sys.argv[1], sys.argv[2]
info = {
    'ubuntu': {'名稱': 'Ubuntu', '架構': 'amd64', '預設登入畫面': 'Ubuntu', 'tags': {'ubuntu', 'deb'}},
    'debian': {'名稱': 'Debian 13', '架構': 'amd64', '預設登入畫面': 'Debian', 'tags': {'debian', 'deb'}},
    'fedora': {'名稱': 'Fedora 44／45', '架構': 'x86_64', '預設登入畫面': 'Fedora Workstation', 'tags': {'fedora', 'rpm'}},
    'rocky': {'名稱': 'Rocky Linux 9', '架構': 'x86_64', '預設登入畫面': 'Rocky Linux', 'tags': {'rocky', 'rpm'}},
}[which]
# 第三個參數是架構（arm64／aarch64 時手冊寫 ARM 電腦）；沒給就用上面的預設（x86）
arch = sys.argv[3] if len(sys.argv) > 3 else ''
info['電腦'] = '64 位元 x86 電腦'
if arch in ('arm64', 'aarch64'):
    info['架構'] = 'arm64（aarch64）' if which != 'fedora' and which != 'rocky' else 'aarch64'
    info['電腦'] = '64 位元 ARM 電腦（例如樹莓派 5、Ampere 伺服器）'
src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'docs', '使用手冊.md.in'), encoding='utf-8').read()

out, keep = [], [True]
for line in src.split('\n'):
    m = re.fullmatch(r'\s*<!--if (\w+)-->\s*', line)
    if m:
        keep.append(keep[-1] and m.group(1) in info['tags'])
        continue
    if re.fullmatch(r'\s*<!--endif-->\s*', line):
        keep.pop()
        continue
    if keep[-1]:
        out.append(line)
text = '\n'.join(out)
text = re.sub(r'\n{3,}', '\n\n', text)   # 拿掉段落後留下的多餘空行
# 日期：這一版在 CHANGELOG.txt 標的發行日期（找不到才用今天）
released = datetime.date.today().isoformat()
try:
    m = re.search(r'^%s 更新（(\d{4}-\d{2}-\d{2})）' % re.escape(version),
                  open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'CHANGELOG.txt'), encoding='utf-8').read(), re.M)
    if m:
        released = m.group(1)
except OSError:
    pass
for k, v in {**info, '版本': version, '日期': released}.items():
    if isinstance(v, str):
        text = text.replace('{{%s}}' % k, v)
assert '{{' not in text and '<!--' not in text, '範本還有沒換掉的標記'
sys.stdout.write(text)
