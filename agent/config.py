import json
import os
from pathlib import Path
from typing import Any, Callable

PROJECT_ROOT = Path(__file__).resolve().parents[1]
CONFIG_PATH = Path(os.getenv("SMARTNAS_CONFIG", PROJECT_ROOT / "config/config.json")).resolve()

try:
    CONFIG = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
except (OSError, ValueError) as exc:
    raise RuntimeError(f"无法加载 SmartNAS 配置: {CONFIG_PATH}: {exc}") from exc


def _value(key: str, env: str, default: Any, cast: Callable = str):
    raw = os.getenv(env, CONFIG.get(key, default))
    if cast is bool:
        return raw if isinstance(raw, bool) else str(raw).lower() not in {"0", "false", "no"}
    return cast(raw)


def _path(key: str, env: str, default: str) -> Path:
    value = Path(_value(key, env, default))
    return value if value.is_absolute() else PROJECT_ROOT / value


CORE_HOST = _value("core_host", "SMARTNAS_CORE_HOST", "0.0.0.0")
CORE_PORT = _value("core_port", "SMARTNAS_CORE_PORT", 8080, int)
AGENT_HOST = _value("agent_host", "SMARTNAS_AGENT_HOST", "0.0.0.0")
AGENT_PORT = _value("agent_port", "SMARTNAS_AGENT_PORT", 8081, int)
NAS_CORE_API = _value("core_api", "SMARTNAS_CORE_API", f"http://127.0.0.1:{CORE_PORT}")

DEEPSEEK_API_KEY = os.getenv("SMARTNAS_DEEPSEEK_API_KEY") or os.getenv("DEEPSEEK_API_KEY") or CONFIG.get("deepseek_api_key", "")
DEEPSEEK_API_BASE = _value("deepseek_api_base", "SMARTNAS_DEEPSEEK_API_BASE", "https://api.deepseek.com").rstrip("/")
DEEPSEEK_MODEL = _value("deepseek_model", "SMARTNAS_DEEPSEEK_MODEL", "deepseek-chat")
DEEPSEEK_TIMEOUT = _value("deepseek_timeout", "SMARTNAS_DEEPSEEK_TIMEOUT", 60, float)
GENERATION_TEMPERATURE = _value("generation_temperature", "SMARTNAS_GENERATION_TEMPERATURE", 0.7, float)
GENERATION_TOP_P = _value("generation_top_p", "SMARTNAS_GENERATION_TOP_P", 0.8, float)

MAX_MARKDOWN_CHARS = _value("max_markdown_chars", "SMARTNAS_MAX_MARKDOWN_CHARS", 12000, int)
SUMMARY_CHUNK_CHARS = _value("summary_chunk_chars", "SMARTNAS_SUMMARY_CHUNK_CHARS", 7000, int)
FILE_QA_DIRECT_CHARS = _value("file_qa_direct_chars", "SMARTNAS_FILE_QA_DIRECT_CHARS", 30000, int)
CACHE_DIR = _path("markdown_cache_dir", "SMARTNAS_CACHE_DIR", "var/cache/markdown")
CACHE_FORMAT_VERSION = 2
TASK_DB_PATH = _path("task_db_path", "SMARTNAS_TASK_DB", "var/cache/agent_tasks.db")
SUMMARY_WORKER_COUNT = _value("summary_worker_count", "SMARTNAS_SUMMARY_WORKER_COUNT", 1, int)

VECTOR_DIR = _path("vector_dir", "SMARTNAS_VECTOR_DIR", "var/cache/vector")
EMBEDDING_MODEL_NAME = _value("embedding_model", "SMARTNAS_EMBEDDING_MODEL", "BAAI/bge-small-zh-v1.5")
RAG_CHUNK_CHARS = _value("rag_chunk_chars", "SMARTNAS_RAG_CHUNK_CHARS", 900, int)
RAG_CHUNK_OVERLAP = _value("rag_chunk_overlap", "SMARTNAS_RAG_CHUNK_OVERLAP", 120, int)
RAG_TOP_K = _value("rag_top_k", "SMARTNAS_RAG_TOP_K", 6, int)
RAG_MIN_SCORE = _value("rag_min_score", "SMARTNAS_RAG_MIN_SCORE", 0.28, float)

AUDIT_LOG_PATH = _path("audit_log_path", "SMARTNAS_AUDIT_LOG", "var/log/agent_audit.jsonl")
AUDIT_LOG_MAX_BYTES = _value("audit_log_max_bytes", "SMARTNAS_AUDIT_LOG_MAX_BYTES", 20 * 1024 * 1024, int)
AUDIT_LOG_BACKUPS = _value("audit_log_backups", "SMARTNAS_AUDIT_LOG_BACKUPS", 10, int)
AUDIT_LOG_FULL_CONTENT = _value("audit_log_full_content", "SMARTNAS_AUDIT_LOG_FULL_CONTENT", True, bool)

