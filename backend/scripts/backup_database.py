#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import sqlite3
import time
from datetime import datetime
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="在线备份 Parking 后台 SQLite")
    parser.add_argument("--database", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--retention-days", type=int, default=7)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source = args.database.expanduser().resolve()
    output = args.output.expanduser().resolve()
    if not source.is_file():
        raise SystemExit(f"数据库不存在: {source}")
    if args.retention_days < 1:
        raise SystemExit("retention-days 必须大于等于 1")

    output.mkdir(parents=True, exist_ok=True)
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    target = output / f"parking-backend-{timestamp}.db"
    temporary = output / f".{target.name}.{os.getpid()}.tmp"
    try:
        with sqlite3.connect(source) as source_db:
            with sqlite3.connect(temporary) as backup_db:
                source_db.backup(backup_db)
                result = backup_db.execute("PRAGMA integrity_check").fetchone()
                if result is None or result[0] != "ok":
                    raise RuntimeError("备份数据库完整性检查失败")
        os.replace(temporary, target)
    finally:
        for suffix in ("", "-wal", "-shm"):
            Path(f"{temporary}{suffix}").unlink(missing_ok=True)

    cutoff = time.time() - args.retention_days * 86400
    removed = 0
    for candidate in output.glob("parking-backend-*.db"):
        if candidate != target and candidate.stat().st_mtime < cutoff:
            candidate.unlink()
            removed += 1

    print(f"备份完成: {target}，清理旧备份 {removed} 个")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
