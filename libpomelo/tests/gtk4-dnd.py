# SPDX-License-Identifier: GPL-3.0-or-later
# gtk4-dnd.py — GTK4（原生 Wayland）的檔案拖放測試視窗（跟小柚瀏覽器同一個工具箱）：
#   上半是拖放目標（收 Gdk.FileList，把路徑與動作印到 stdout）、下半是來源（按住拖出 GTK4_DND_FILES=路徑:路徑）。
#   用法：GDK_BACKEND=wayland python3 gtk4-dnd.py
import gi, os, sys
gi.require_version('Gtk', '4.0')
from gi.repository import Gtk, Gdk, Gio, GLib


def act(app):
    w = Gtk.ApplicationWindow(application=app, title="GTK4 DND")
    w.set_default_size(420, 300)
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
    dst = Gtk.Label(label="TARGET 放這裡", vexpand=True)
    dst.add_css_class("title-2")
    src = Gtk.Label(label="SOURCE 從這裡拖", vexpand=True)
    box.append(dst)
    box.append(src)
    w.set_child(box)

    t = Gtk.DropTarget.new(Gdk.FileList, Gdk.DragAction.COPY | Gdk.DragAction.MOVE)
    def on_drop(tgt, value, x, y):
        for f in value.get_files():
            print("GTK4 drop:", f.get_path(), flush=True)
        return True
    t.connect("drop", on_drop)
    t.connect("enter", lambda tgt, x, y: (print("GTK4 enter", flush=True), Gdk.DragAction.COPY)[1])
    dst.add_controller(t)

    ds = Gtk.DragSource.new()
    ds.set_actions(Gdk.DragAction.COPY | Gdk.DragAction.MOVE)
    def prep(source, x, y):
        files = [Gio.File.new_for_path(p) for p in os.environ.get("GTK4_DND_FILES", "/etc/hostname").split(":")]
        fl = Gdk.FileList.new_from_list(files)
        return Gdk.ContentProvider.new_for_value(fl)
    ds.connect("prepare", prep)
    ds.connect("drag-end", lambda s, d, d2: print("GTK4 drag-end", flush=True))
    src.add_controller(ds)
    w.present()


app = Gtk.Application(application_id="tw.pomelo.Gtk4Dnd")
app.connect("activate", act)
app.run(None)
