#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""fake-qemu.py — 測試用的假 QEMU：一個最小的 VNC 伺服器（RFB 3.8、Raw 編碼、游標、QEMU 延伸按鍵）加 QMP（ctl／qmp 兩個 socket）。
給 pomelo-vm 的畫面視窗（--console）做端到端測試，不用真的虛擬機器、不碰使用者的虛擬機器。

用法：fake-qemu.py ctl.sock路徑 qmp.sock路徑 vnc.sock路徑 記錄檔 [寬 高] [剪貼簿文字]
  pomelo-vm 用 $XDG_RUNTIME_DIR/pomelo-vm/<id>/ 找 qemu.pid、ctl.sock、qmp.sock、vnc.sock，而且 qemu.pid 的行程命令列要含 ctl.sock 的路徑，
  所以把三個路徑都放在命令列上；啟動時會寫 qemu.pid（在 ctl.sock 的資料夾）。
畫面：深色背景＋網格＋四角色塊，滑鼠位置畫一個紅色方塊，最近的事件列在左上角；游標形狀是黃圈十字（熱點在中心）。
記錄檔：KEY 掃描碼 down|up（QEMU 的編號，0xe0 開頭的延伸鍵是 0x80|碼）、PTR 按鍵遮罩 x y、CUT 文字、CURSOR-ENC 有沒有送游標。"""
import os, selectors, socket, struct, sys, json, time
import cairo

ctl_path, qmp_path, vnc_path, log_path = sys.argv[1:5]
W = int(sys.argv[5]) if len(sys.argv) > 5 else 800
H = int(sys.argv[6]) if len(sys.argv) > 6 else 600
CLIP = sys.argv[7] if len(sys.argv) > 7 else ""
logf = open(log_path, "a", buffering=1)
lines = []
state = {"x": 100, "y": 100, "dirty": True, "running": True}


def log(s):
    logf.write(s + "\n")
    lines.append(s)
    del lines[:-9]
    state["dirty"] = True


def frame():
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)
    cr = cairo.Context(surf)
    cr.set_source_rgb(0.08, 0.18, 0.12)
    cr.paint()
    cr.set_source_rgba(1, 1, 1, 0.12)
    for x in range(0, W, 64):
        cr.rectangle(x, 0, 1, H)
    for y in range(0, H, 64):
        cr.rectangle(0, y, W, 1)
    cr.fill()
    for (cx, cy, col) in ((0, 0, (1, .3, .3)), (W - 40, 0, (.3, 1, .3)), (0, H - 40, (.3, .5, 1)), (W - 40, H - 40, (1, 1, .3))):
        cr.set_source_rgb(*col)
        cr.rectangle(cx, cy, 40, 40)
        cr.fill()
    cr.set_source_rgb(1, 1, 1)
    cr.select_font_face("monospace", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
    cr.set_font_size(26)
    cr.move_to(60, 60)
    cr.show_text("FAKE QEMU VNC %dx%d" % (W, H))
    cr.set_font_size(17)
    for i, s in enumerate(lines):
        cr.move_to(60, 100 + i * 24)
        cr.show_text(s[:70])
    cr.set_source_rgb(1, 0.1, 0.1)
    cr.rectangle(state["x"] - 8, state["y"] - 8, 16, 16)
    cr.fill()
    surf.flush()
    return bytes(surf.get_data())


def cursor_rect():
    """Cursor 偽編碼：32×32 的黃圈十字，像素用目前的像素格式（BGRX）＋遮罩（每行 4 位元組，最高位是最左邊）"""
    n = 32
    px = bytearray()
    mask = bytearray()
    for y in range(n):
        row = 0
        for x in range(n):
            d = (x - 15.5) ** 2 + (y - 15.5) ** 2
            cross = x in (15, 16) or y in (15, 16)
            if 11 * 11 < d < 14 * 14:
                px += bytes((0, 220, 255, 0)); row |= 0x80000000 >> x
            elif cross and d < 11 * 11:
                px += bytes((0, 0, 255, 0)); row |= 0x80000000 >> x
            else:
                px += bytes((0, 0, 0, 0))
        mask += struct.pack(">I", row)
    return struct.pack(">HHHHi", 16, 16, n, n, -239) + bytes(px) + bytes(mask)


class Vnc:
    def __init__(self, sock):
        self.s = sock
        self.buf = b""
        self.stage = 0
        self.want_update = False
        self.ext_keys = False
        self.cursor_enc = False
        self.sent_cursor = False
        self.sent_clip = False
        self.s.sendall(b"RFB 003.008\n")

    def send_update(self):
        rects = []
        if self.ext_keys and not getattr(self, "sent_ext", False):
            rects.append(struct.pack(">HHHHi", 0, 0, 0, 0, -258))   # QEMU：告訴用戶端支援延伸按鍵事件
            self.sent_ext = True
        if self.cursor_enc and not self.sent_cursor:
            rects.append(cursor_rect())
            self.sent_cursor = True
        data = frame()
        rects.append(struct.pack(">HHHHi", 0, 0, W, H, 0) + data)
        self.s.sendall(struct.pack(">BxH", 0, len(rects)) + b"".join(rects))
        self.want_update = False
        state["dirty"] = False

    def feed(self, data):
        self.buf += data
        while True:
            if self.stage == 0:   # 版本
                if len(self.buf) < 12: return
                self.buf = self.buf[12:]
                self.s.sendall(bytes((1, 1)))   # 一種安全型態：None
                self.stage = 1
            elif self.stage == 1:
                if len(self.buf) < 1: return
                self.buf = self.buf[1:]
                self.s.sendall(struct.pack(">I", 0))
                self.stage = 2
            elif self.stage == 2:   # ClientInit
                if len(self.buf) < 1: return
                self.buf = self.buf[1:]
                pf = struct.pack(">BBBBHHHBBBxxx", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
                name = b"fake-qemu"
                self.s.sendall(struct.pack(">HH", W, H) + pf + struct.pack(">I", len(name)) + name)
                self.stage = 3
            else:
                if not self.buf: return
                t = self.buf[0]
                if t == 0:
                    need = 20
                elif t == 2:
                    if len(self.buf) < 4: return
                    need = 4 + 4 * struct.unpack(">H", self.buf[2:4])[0]
                elif t == 3:
                    need = 10
                elif t == 4:
                    need = 8
                elif t == 5:
                    need = 6
                elif t == 6:
                    if len(self.buf) < 8: return
                    need = 8 + struct.unpack(">I", self.buf[4:8])[0]
                elif t == 255:
                    if len(self.buf) < 2: return
                    need = 12 if self.buf[1] == 0 else 2
                elif t == 251:
                    need = 24
                else:
                    log("UNKNOWN-MSG %d" % t); self.buf = b""; return
                if len(self.buf) < need: return
                m, self.buf = self.buf[:need], self.buf[need:]
                self.msg(t, m)

    def msg(self, t, m):
        if t == 2:
            n = struct.unpack(">H", m[2:4])[0]
            encs = struct.unpack(">%di" % n, m[4:])
            self.ext_keys = -258 in encs
            self.cursor_enc = -239 in encs
            log("ENCODINGS ext-key=%d cursor=%d" % (self.ext_keys, self.cursor_enc))
        elif t == 3:
            self.want_update = True
        elif t == 4:
            log("KEYSYM 0x%x %s" % (struct.unpack(">I", m[4:8])[0], "down" if m[1] else "up"))
        elif t == 5:
            mask, x, y = struct.unpack(">BHH", m[1:6])
            state["x"], state["y"] = x, y
            if getattr(self, "last_mask", 0) != mask:
                log("PTR 0x%02x %d %d" % (mask, x, y))
                self.last_mask = mask
            state["dirty"] = True
        elif t == 6:
            n = struct.unpack(">I", m[4:8])[0]
            log("CUT %s" % m[8:8 + n].decode("latin-1"))
        elif t == 255 and m[1] == 0:
            down, ks, sc = struct.unpack(">HII", m[2:12])
            log("KEY 0x%x %s" % (sc, "down" if down else "up"))
        elif t == 251:
            log("SETDESKTOPSIZE")


def qmp_serve(conn):
    conn.sendall(b'{"QMP": {"version": {"qemu": {"micro": 2, "minor": 2, "major": 8}, "package": ""}, "capabilities": []}}\n')
    buf = b""
    conn.settimeout(None)
    while True:
        d = conn.recv(4096)
        if not d:
            return
        buf += d
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            try:
                req = json.loads(line)
            except Exception:
                continue
            c = req.get("execute", "")
            if c == "query-version":
                r = {"return": {"qemu": {"major": 8, "minor": 2, "micro": 2}, "package": ""}}
            elif c == "query-status":
                r = {"return": {"status": "running", "running": True}}
            else:
                r = {"return": {}}
            conn.sendall((json.dumps(r) + "\n").encode())


def main():
    rundir = os.path.dirname(ctl_path)
    with open(os.path.join(rundir, "qemu.pid"), "w") as f:
        f.write(str(os.getpid()))
    socks = {}
    sel = selectors.DefaultSelector()
    for name, p in (("ctl", ctl_path), ("qmp", qmp_path), ("vnc", vnc_path)):
        if os.path.exists(p):
            os.unlink(p)
        s = socket.socket(socket.AF_UNIX)
        s.bind(p)
        s.listen(4)
        s.setblocking(False)
        sel.register(s, selectors.EVENT_READ, name)
    vncs = {}
    import threading
    last_clip = 0
    while True:
        for key, _ in sel.select(0.1):
            if key.data in ("ctl", "qmp"):
                c, _ = key.fileobj.accept()
                threading.Thread(target=lambda c=c: (qmp_serve(c) if True else None), daemon=True).start()
            elif key.data == "vnc":
                c, _ = key.fileobj.accept()
                v = Vnc(c)
                vncs[c] = v
                sel.register(c, selectors.EVENT_READ, "vnc-client")
            else:
                c = key.fileobj
                try:
                    d = c.recv(65536)
                except OSError:
                    d = b""
                if not d:
                    sel.unregister(c); vncs.pop(c, None); c.close(); continue
                vncs[c].feed(d)
        for c, v in list(vncs.items()):
            if v.want_update and state["dirty"] and v.stage == 3:
                try:
                    v.send_update()
                except OSError:
                    sel.unregister(c); vncs.pop(c, None); c.close(); continue
            if CLIP and v.stage == 3 and not v.sent_clip and v.cursor_enc and time.time() - last_clip > 1:
                v.sent_clip = True
                t = CLIP.encode("latin-1", "replace")
                v.s.sendall(struct.pack(">BxxxI", 3, len(t)) + t)
                log("CUT-OFFER %s" % CLIP)


main()
