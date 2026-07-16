"""命令函数实现"""

from datetime import date
from .models import Habit, calc_streak, calc_completion_rate
from .storage import list_habits, find_habit, add_habit, update_habit, delete_habit, show_stats


def cmd_add():
    from .cli import _header
    _header("创建新习惯")
    name = input("名称: ").strip()
    if not name:
        print("名称不能为空"); return
    freq = "weekly" if input("频率 1每日 2每周 [1]: ").strip() == "2" else "daily"
    desc = input("描述: ").strip()
    tags = [t.strip() for t in input("标签(逗号分隔): ").strip().split(",") if t.strip()]
    h = Habit(name=name, frequency=freq, description=desc, tags=tags)
    add_habit(h)
    print(f"✅ 创建成功 {h.id}")


def cmd_list():
    habits = list_habits()
    if not habits:
        print("\n📭 还没有习惯"); return
    from .cli import _header
    _header(f"我的习惯 ({len(habits)})")
    for h in habits:
        s = calc_streak(h._checkins, h.frequency)
        fl = "📅每日" if h.frequency == "daily" else "📆每周"
        tags = f" [{', '.join(h.tags)}]" if h.tags else ""
        r = calc_completion_rate(h._checkins, h.frequency)
        bar = "█" * int(20 * r / 100) + "░" * (20 - int(20 * r / 100))
        print(f"\n[{h.id}] {h.name}{tags}")
        print(f"  {fl} 🔥{s}天 共{len(h._checkins)}次")
        print(f"  近30天: {bar} {r:.0f}%")


def cmd_checkin(habit_id):
    if not habit_id:
        print("用法: checkin <id>"); return
    h = find_habit(habit_id)
    if not h:
        print(f"未找到 {habit_id}"); return
    today = date.today().isoformat()
    if today in h._checkins:
        print("今天已打卡"); return
    h._checkins.append(today)
    update_habit(h)
    s = calc_streak(h._checkins, h.frequency)
    print(f"✅ {h.name} 打卡成功! 连续{s}天")
    if s >= 30: print("💪 超过一个月！")
    elif s >= 14: print("👏 坚持两周！")
    elif s >= 7: print("🌟 坚持一周！")


def cmd_show(habit_id):
    if not habit_id:
        print("用法: show <id>"); return
    h = find_habit(habit_id)
    if not h:
        print(f"未找到 {habit_id}"); return
    from .cli import _header
    _header(f"详情: {h.name}")
    print(f"  ID: {h.id}\n  名称: {h.name}\n  频率: {'每日' if h.frequency=='daily' else '每周'}")
    print(f"  描述: {h.description or '无'}\n  标签: {', '.join(h.tags) or '无'}")
    print(f"  总打卡: {len(h._checkins)}\n  连续: {calc_streak(h._checkins, h.frequency)}天")
    print(f"  完成率: {calc_completion_rate(h._checkins, h.frequency)}%")
    if h._checkins:
        print("  最近打卡:")
        for d in sorted(h._checkins, reverse=True)[:10]:
            print(f"    {d}")


def cmd_delete(habit_id):
    if not habit_id:
        print("用法: delete <id>"); return
    h = find_habit(habit_id)
    if not h:
        print(f"未找到 {habit_id}"); return
    if input(f"确认删除 '{h.name}'? (y/n): ").strip().lower() in ("y", "yes"):
        print("已删除" if delete_habit(habit_id) else "删除失败")


def cmd_export():
    habits = list_habits()
    if not habits:
        print("无数据"); return
    from .cli import _header
    _header("导出")
    for h in habits:
        print(f"\n[{h.id}] {h.name} ({h.frequency})")
        cs = ', '.join(sorted(h._checkins)) if h._checkins else '(无)'
        print(f"  打卡: {cs}")


def cmd_streak():
    habits = list_habits()
    if not habits:
        print("无习惯"); return
    from .cli import _header
    _header("🔥 连续打卡排行")
    ranked = sorted(
        [(calc_streak(h._checkins, h.frequency), h.name, h.id, h.frequency) for h in habits],
        reverse=True
    )
    medals = {0:"🥇",1:"🥈",2:"🥉"}
    for i, (s, name, hid, freq) in enumerate(ranked):
        m = medals.get(i, "  ")
        print(f"  {m} {'📅' if freq=='daily' else '📆'} [{hid}] {name} → 🔥{s}天")
