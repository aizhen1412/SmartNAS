"""SQLite persistence for long-running Agent tasks.

Task execution stays in the service layer, while this module owns the durable
state shared by summary and index jobs.  Tokens are deliberately not stored:
an interrupted task is reported as failed and can be safely re-submitted after
the user authenticates again.
"""

import json
import sqlite3
import time
from pathlib import Path
from typing import Any, Dict, List


class TaskStore:
    def __init__(self, path: Path):
        self.path = path

    def initialize(self) -> Dict[str, List[Dict[str, Any]]]:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with sqlite3.connect(self.path) as db:
            db.execute(
                """
                CREATE TABLE IF NOT EXISTS agent_tasks (
                    id TEXT PRIMARY KEY,
                    owner TEXT NOT NULL,
                    file_hash TEXT NOT NULL,
                    force INTEGER NOT NULL DEFAULT 0,
                    status TEXT NOT NULL,
                    message TEXT,
                    result_json TEXT,
                    error TEXT,
                    created_at REAL NOT NULL,
                    updated_at REAL NOT NULL,
                    task_type TEXT NOT NULL DEFAULT 'summary',
                    options_json TEXT
                )
                """
            )
            columns = {row[1] for row in db.execute("PRAGMA table_info(agent_tasks)")}
            if "task_type" not in columns:
                db.execute("ALTER TABLE agent_tasks ADD COLUMN task_type TEXT NOT NULL DEFAULT 'summary'")
            if "options_json" not in columns:
                db.execute("ALTER TABLE agent_tasks ADD COLUMN options_json TEXT")
            db.execute(
                "UPDATE agent_tasks SET status = 'failed', message = 'Agent 重启导致任务中断', "
                "error = 'interrupted', updated_at = ? "
                "WHERE status IN ('pending', 'running', 'cancel_requested')",
                (time.time(),),
            )
            rows = db.execute(
                "SELECT id, owner, file_hash, force, status, message, result_json, error, "
                "created_at, updated_at, task_type, options_json "
                "FROM agent_tasks ORDER BY created_at DESC LIMIT 500"
            ).fetchall()

        tasks: Dict[str, List[Dict[str, Any]]] = {"summary": [], "index": []}
        for row in reversed(rows):
            task_type = row[10] if row[10] in tasks else "summary"
            task = {
                "id": row[0],
                "owner": row[1],
                "hash": row[2],
                "force": bool(row[3]),
                "status": row[4],
                "message": row[5] or "",
                "result": json.loads(row[6]) if row[6] else None,
                "error": row[7],
                "created_at": row[8],
                "updated_at": row[9],
            }
            task.update(json.loads(row[11]) if row[11] else {})
            tasks[task_type].append(task)
        return tasks

    def save(self, task: Dict[str, Any], task_type: str) -> None:
        if task_type not in {"summary", "index"}:
            raise ValueError(f"unsupported task type: {task_type}")
        options = {
            key: value for key, value in task.items()
            if key not in {"id", "owner", "hash", "force", "status", "message", "result", "error", "created_at", "updated_at"}
        }
        with sqlite3.connect(self.path) as db:
            db.execute(
                """
                INSERT INTO agent_tasks
                    (id, owner, file_hash, force, status, message, result_json, error,
                     created_at, updated_at, task_type, options_json)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                ON CONFLICT(id) DO UPDATE SET
                    status = excluded.status,
                    message = excluded.message,
                    result_json = excluded.result_json,
                    error = excluded.error,
                    updated_at = excluded.updated_at,
                    task_type = excluded.task_type,
                    options_json = excluded.options_json
                """,
                (
                    task["id"], task["owner"], task.get("hash", ""), int(task.get("force", False)),
                    task["status"], task.get("message", ""),
                    json.dumps(task.get("result"), ensure_ascii=False) if task.get("result") is not None else None,
                    task.get("error"), task["created_at"], task["updated_at"], task_type,
                    json.dumps(options, ensure_ascii=False) if options else None,
                ),
            )
