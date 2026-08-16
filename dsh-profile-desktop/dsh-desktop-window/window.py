#!/usr/bin/env python3
"""dsh-desktop-window: minimal GTK WebKit shell for the dsh web GUI.

Polls the target URL until the dsh webserver answers, then loads it in a
chromeless window. Closing the window exits the process; the host plugin
turns that into the dsh shutdown signal.
"""
import os
import sys
import urllib.request

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("WebKit2", "4.0")
from gi.repository import GLib, Gtk, WebKit2  # noqa: E402

URL = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:3080"
TITLE = "DeepSeek Harness"
ICON = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icon.svg")

GLib.set_prgname("dsh-desktop")
GLib.set_application_name(TITLE)

if os.path.exists(ICON):
    try:
        Gtk.Window.set_default_icon_from_file(ICON)
    except Exception:
        pass


def server_ready() -> bool:
    try:
        with urllib.request.urlopen(URL, timeout=1):
            return True
    except Exception:
        return False


def main() -> None:
    win = Gtk.Window(title=TITLE)
    win.set_default_size(1440, 900)
    win.set_position(Gtk.WindowPosition.CENTER)

    view = WebKit2.WebView()
    settings = view.get_settings()
    settings.set_property("enable-developer-extras", True)
    win.add(view)

    win.connect("destroy", Gtk.main_quit)
    win.show_all()

    def poll() -> bool:
        if server_ready():
            view.load_uri(URL)
            return False  # stop polling
        return True  # keep polling

    GLib.timeout_add(300, poll)
    Gtk.main()


if __name__ == "__main__":
    main()
