"""数据持久化 - JSON 文件存储"""

import json
import os
from pathlib import Path
from typing import Optional

from .models import Habit

DATA_DIR = Path.home() / ".habit_tracker"
DATA_FILE = DATA_DIR / "habits.json"


def _ensure_dir():
    """确保数据目录存在"""
    DATA_DIR.mkdir(parents=True, exist_ok=True)


def _load_all() -> list[dict]:
    """从 JSON 文件加载全部数据"""
    _ensure_dir()
    if not DATA_FILE.exists():
        return []
    try:
        with open(DATA_FILE, "r", encoding="utf-8") as f:
            data = json.load(f)
            return data if isinstance(data, list) else []
    except (json.JSONDecodeError, IOError):
        return []


def _save_all(records: list[dict]):
    """将全部数据写入 JSON 文件"""
    _ensure_dir()
    with open(DATA_FILE, "w", encoding="utf-8") as f:
        json.dump(records, f, ensure_ascii=False, indent=2)


def list_habits() -> list[Habit]:
    """列出所有习惯"""
    return [Habit.from_dict(item) for item in _load_all()]


def find_habit(habit_id: str) -> Optional[Habit]:
    """根据 ID 查找习惯"""
    for item in _load_all():
        if item.get("id") == habit_id:
            return Habit.from_dict(item)
    return None


def add_habit(habit: Habit):
    """添加新习惯"""
    records = _load_all()
    # 重名检查
    for item in records:
        if item.get("name") == habit.name:
            print(f"⚠️  同名习惯 '{habit.name}' 已存在，将添加为新习惯")
    records.append(habit.to_dict())
    _save_all(records)


def update_habit(habit: Habit):
    """更新习惯数据（覆盖式）"""
    records = _load_all()
    for i, item in enumerate(records):
        if item.get("id") == habit.id:
            records[i] = habit.to_dict()
            break
    _save_all(records)


def delete_habit(habit_id: str) -> bool:
    """删除习惯，返回是否成功"""
    records = _load_all()
    new_records = [r for r in records if r.get("id") != habit_id]
    if len(new_records) == len(records):
        return False
    _save_all(new_records)
    return True


def show_stats():
    """显示全局统计信息"""
    habits = list_habits()
    if not habits:
        print("📭 还没有任何习惯，开始创建吧！")
        return

    total = len(habits)
    total_checkins = sum(len(h._checkins) for h in habits)
    active = sum(1 for h in habits if h._checkins)

    print(f"📊 全局统计")
    print(f"  习惯总数: {total}")
    print(f"  有过打卡: {active}")
    print(f"  总打卡数: {total_checkins}")
    print(f"  平均打卡: {total_checkins / total:.1f} 次/习惯" if total else "")
