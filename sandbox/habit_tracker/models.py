"""数据模型定义"""

from dataclasses import dataclass, field
from datetime import date, timedelta
from typing import Optional
import uuid


@dataclass
class Habit:
    """单个习惯的数据模型"""
    name: str
    frequency: str = "daily"
    description: str = ""
    tags: list[str] = field(default_factory=list)
    created_at: str = ""
    id: str = ""

    _checkins: list[str] = field(default_factory=list)

    def __post_init__(self):
        if not self.id:
            self.id = uuid.uuid4().hex[:8]
        if not self.created_at:
            self.created_at = date.today().isoformat()

    def to_dict(self) -> dict:
        return {
            "id": self.id,
            "name": self.name,
            "frequency": self.frequency,
            "description": self.description,
            "tags": self.tags,
            "created_at": self.created_at,
            "checkins": sorted(set(self._checkins)),
        }

    @classmethod
    def from_dict(cls, data: dict) -> "Habit":
        habit = cls(
            name=data["name"],
            frequency=data.get("frequency", "daily"),
            description=data.get("description", ""),
            tags=data.get("tags", []),
            created_at=data.get("created_at", ""),
            id=data.get("id", ""),
        )
        habit._checkins = data.get("checkins", [])
        return habit


def calc_streak(checkins: list[str], frequency: str) -> int:
    """根据打卡记录计算当前连续天数/周数"""
    if not checkins:
        return 0

    dates = sorted(set(checkins))
    today = date.today()
    checkin_set = set(dates)

    if frequency == "daily":
        streak = 0
        d = today
        for _ in range(366):
            if d.isoformat() in checkin_set:
                streak += 1
                d -= timedelta(days=1)
            else:
                break
        return streak

    elif frequency == "weekly":
        current_week_start = today - timedelta(days=today.weekday())
        streak = 0
        for _ in range(53):
            ws = current_week_start.isoformat()
            if ws in checkin_set:
                streak += 1
                current_week_start -= timedelta(days=7)
            else:
                break
        return streak

    return 0


def calc_completion_rate(checkins: list[str], frequency: str, days_back: int = 30) -> float:
    """计算最近 days_back 天内的完成率"""
    today = date.today()
    checkin_set = set(checkins)

    if frequency == "daily":
        total = days_back
        done = 0
        for i in range(days_back):
            d = (today - timedelta(days=i)).isoformat()
            if d in checkin_set:
                done += 1
        return round(done / total * 100, 1) if total > 0 else 0.0

    elif frequency == "weekly":
        total = max(days_back // 7, 4)
        done = 0
        for i in range(total):
            ws = (today - timedelta(days=today.weekday()) - timedelta(weeks=i)).isoformat()
            if ws in checkin_set:
                done += 1
        return round(done / total * 100, 1) if total > 0 else 0.0

    return 0.0
