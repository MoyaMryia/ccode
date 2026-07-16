"""CLI 入口和交互循环"""

import sys, shutil
from .commands import *
from .storage import show_stats


def _tw():
    return shutil.get_terminal_size((80, 20)).columns


def _header(title):
    w = min(_tw(), 60)
    t = f" {title} "
    s = (w - len(t)) // 2
    print(f"\n{'=' * max(s, 0)}{t}{'=' * max(s, 0)}")


def _help():
    print("""
命令: add, list, checkin <id>, show <id>, delete <id>, stats, streak, export, quit
""")


def run_interactive():
    _header("🌟 习惯追踪器")
    print("输入 help 查看命令, quit 退出")
    while True:
        try:
            line = input("\n⏺ ").strip()
            if not line: continue
            parts = line.split(maxsplit=1)
            cmd, arg = parts[0].lower(), (parts[1] if len(parts) > 1 else "")
            if cmd in ("q","quit","exit"):
                print("👋 再见！"); break
            elif cmd == "help": _help()
            elif cmd == "add": cmd_add()
            elif cmd in ("list","ls"): cmd_list()
            elif cmd in ("checkin","check","done"): cmd_checkin(arg)
            elif cmd in ("show","view"): cmd_show(arg)
            elif cmd in ("delete","del"): cmd_delete(arg)
            elif cmd == "stats": show_stats()
            elif cmd == "streak": cmd_streak()
            elif cmd == "export": cmd_export()
            else: print(f"未知命令 '{cmd}'")
        except KeyboardInterrupt:
            print("\n👋 再见！"); break
        except Exception as e:
            print(f"❌ {e}")


def main():
    if len(sys.argv) > 1:
        cmd, arg = sys.argv[1].lower(), (sys.argv[2] if len(sys.argv) > 2 else "")
        m = {
            "add": lambda: cmd_add(), "list": cmd_list, "ls": cmd_list,
            "checkin": lambda: cmd_checkin(arg), "show": lambda: cmd_show(arg),
            "delete": lambda: cmd_delete(arg), "stats": show_stats, "streak": cmd_streak,
            "export": cmd_export, "help": _help,
        }
        if cmd in ("-v","--version"):
            print(f"habit_tracker v{__import__('habit_tracker').__version__}")
        elif cmd in m:
            m[cmd]()
        else:
            print(f"未知命令 '{cmd}'")
    else:
        run_interactive()
