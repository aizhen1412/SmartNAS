import uvicorn
from fastapi import FastAPI, Header, HTTPException
from fastapi.middleware.cors import CORSMiddleware
import json
import logging
import os
import requests
import re
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from logging.handlers import RotatingFileHandler
from pathlib import Path
from typing import Dict, List, Optional, Any

from .config import (
    AUDIT_LOG_BACKUPS,
    AUDIT_LOG_FULL_CONTENT,
    AUDIT_LOG_MAX_BYTES,
    AUDIT_LOG_PATH,
    AGENT_HOST,
    AGENT_PORT,
    DEEPSEEK_API_BASE,
    DEEPSEEK_API_KEY,
    DEEPSEEK_MODEL,
    FILE_QA_DIRECT_CHARS,
    GENERATION_TEMPERATURE,
    GENERATION_TOP_P,
    MAX_MARKDOWN_CHARS,
    NAS_CORE_API,
    RAG_CHUNK_CHARS,
    RAG_TOP_K,
    SUMMARY_CHUNK_CHARS,
    SUMMARY_WORKER_COUNT,
    TASK_DB_PATH,
)
from .core_client import fetch_all_user_files, get_user_identity
from .index_service import compute_missing_index_files
from .keyword_store import keyword_index_status, mark_keyword_index_dirty, rebuild_keyword_index
from .llm_client import (
    clean_model_text,
    create_chat_completion,
    create_chat_message,
    stream_chat_completion,
)
from .markdown_service import MarkdownService
from .rag_store import (
    health_status as rag_health_status,
    index_markdown_for_rag,
    index_status as rag_index_status,
    rag_has_file_index,
    set_audit_callback as set_rag_audit_callback,
)
from .retrieval_service import RetrievalFilters, search_documents
from .schemas import ChatRequest, FileQuestionRequest, IndexRebuildRequest, RagQueryRequest, SummarizeRequest
from .sse import sse_event, sse_response
from .task_store import TaskStore

try:
    from markitdown import MarkItDown
except Exception as exc:
    MarkItDown = None
    MARKITDOWN_IMPORT_ERROR = exc
else:
    MARKITDOWN_IMPORT_ERROR = None

app = FastAPI(title="SmartNAS Agent Service")

# 添加 CORS 支持，允许所有来源跨域
app.add_middleware(
    CORSMiddleware,
    allow_origin_regex=r"https?://(localhost|127\.0\.0\.1|\[::1\]|10\.\d+\.\d+\.\d+|192\.168\.\d+\.\d+|172\.(1[6-9]|2\d|3[0-1])\.\d+\.\d+)(:\d+)?",
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

markitdown_converter = None
summary_executor = ThreadPoolExecutor(max_workers=SUMMARY_WORKER_COUNT)
summary_tasks: Dict[str, Dict[str, Any]] = {}
summary_tasks_lock = threading.Lock()
task_cancel_events: Dict[str, threading.Event] = {}
index_tasks: Dict[str, Dict[str, Any]] = {}
index_tasks_lock = threading.Lock()
task_store = TaskStore(TASK_DB_PATH)
audit_logger = logging.getLogger("smartnas.agent.audit")

def init_audit_logger() -> None:
    AUDIT_LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    audit_logger.setLevel(logging.INFO)
    audit_logger.propagate = False
    if audit_logger.handlers:
        return
    handler = RotatingFileHandler(
        AUDIT_LOG_PATH,
        maxBytes=AUDIT_LOG_MAX_BYTES,
        backupCount=AUDIT_LOG_BACKUPS,
        encoding="utf-8",
    )
    handler.setFormatter(logging.Formatter("%(message)s"))
    audit_logger.addHandler(handler)
    try:
        os.chmod(AUDIT_LOG_PATH, 0o600)
    except OSError:
        pass

def audit_event(user: str, operation: str, status: str, **details: Any) -> None:
    record = {
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "user": user or "unknown",
        "operation": operation,
        "status": status,
        "thread": threading.current_thread().name,
        "details": details,
    }
    audit_logger.info(json.dumps(record, ensure_ascii=False, default=str))
    try:
        os.chmod(AUDIT_LOG_PATH, 0o600)
    except OSError:
        pass

def audit_content(value: str) -> str:
    if AUDIT_LOG_FULL_CONTENT or len(value) <= 2000:
        return value
    return value[:2000] + f"\n...[审计内容已截断，原始字符数 {len(value)}]"

init_audit_logger()
set_rag_audit_callback(audit_event)

@app.middleware("http")
async def audit_http_request(request, call_next):
    started_at = time.time()
    authorization = request.headers.get("authorization", "")
    user = "anonymous"
    if authorization.startswith("Bearer "):
        try:
            user = get_user_identity(authorization)
        except Exception:
            user = "invalid-token"
    try:
        response = await call_next(request)
        audit_event(
            user,
            "http_request",
            "success" if response.status_code < 400 else "failed",
            method=request.method,
            path=request.url.path,
            query=request.url.query,
            status_code=response.status_code,
            duration_ms=round((time.time() - started_at) * 1000),
        )
        return response
    except Exception as exc:
        audit_event(
            user,
            "http_request",
            "failed",
            method=request.method,
            path=request.url.path,
            query=request.url.query,
            duration_ms=round((time.time() - started_at) * 1000),
            error=str(exc),
            exception_type=type(exc).__name__,
        )
        raise

SUPPORTED_SUMMARY_EXTENSIONS = {
    ".pdf",
    ".docx",
    ".pptx",
    ".xlsx",
    ".txt",
    ".csv",
    ".json",
    ".html",
    ".htm",
    ".md",
    ".xml",
    ".epub",
    ".zip",
    ".ipynb",
    ".jpg",
    ".jpeg",
    ".png",
    ".webp",
    ".gif",
    ".bmp",
    ".tiff",
    ".tif",
    ".svg",
    ".wav",
    ".mp3",
    ".m4a",
    ".mp4",
}

IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp", ".tiff", ".tif"}
TEXT_LIKE_EXTENSIONS = {".txt", ".csv", ".json", ".html", ".htm", ".md", ".xml", ".svg"}
MEDIA_EXTENSIONS = {".wav", ".mp3", ".m4a", ".mp4"}

sessions: Dict[str, List[dict]] = {}
file_qa_sessions: Dict[str, List[dict]] = {}
system_prompt = """你是 SmartNAS 的智能管家。优先根据已索引文件内容回答问题。
当用户需要按文件名、摘要或标签查找文件，而现有上下文不足时，调用 search_files 工具。
不要编造文件内容；工具和检索结果没有依据时要明确说明。"""

SEARCH_FILE_TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "search_files",
            "description": "根据文件名、摘要或标签搜索当前用户的 SmartNAS 文件",
            "parameters": {
                "type": "object",
                "properties": {"keyword": {"type": "string", "description": "搜索关键词"}},
                "required": ["keyword"],
            },
        },
    }
]

QUERY_ROUTING_TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "choose_query_route",
            "description": "选择回答当前 SmartNAS 问题所需的检索方式",
            "parameters": {
                "type": "object",
                "properties": {
                    "route": {
                        "type": "string",
                        "enum": ["catalog", "rag"],
                        "description": "catalog 用于文件是否存在、文件列表、目录和文件名；rag 用于文件内容、主题、事实和总结",
                    },
                },
                "required": ["route"],
            },
        },
    }
]

def choose_query_route(prompt: str) -> str:
    decision = create_chat_message(
        [
            {
                "role": "system",
                "content": (
                    "你是 SmartNAS 查询路由器，必须调用 choose_query_route。"
                    "查询文件是否存在、文件名、目录、列表或‘索引了什么文件’时选 catalog；"
                    "查询文件内容、主题、事实、比较、总结时选 rag。"
                ),
            },
            {"role": "user", "content": prompt},
        ],
        QUERY_ROUTING_TOOLS,
    )
    for call in decision.get("tool_calls") or []:
        function = call.get("function") or {}
        if function.get("name") != "choose_query_route":
            continue
        try:
            route = json.loads(function.get("arguments") or "{}").get("route")
        except ValueError:
            route = None
        if route in {"catalog", "rag"}:
            return route
    return "rag"

def build_file_catalog_messages(question: str, token: str) -> List[dict]:
    files = fetch_all_user_files(token)
    catalog = [
        {
            "name": item.get("name") or "",
            "directory": item.get("directory") or "/",
        }
        for item in files
        if item.get("name")
    ]
    return [
        {
            "role": "system",
            "content": (
                "你是 SmartNAS 的文件目录助手。下面的目录是当前用户全部可访问文件的唯一事实来源。"
                "只根据目录回答文件是否存在、有哪些文件或位于什么路径；不要把文件内容检索结果当作目录。"
                "可以识别常见的中英文书名翻译，例如《理想国》对应 The Republic。"
                f"\n\n文件总数：{len(catalog)}\n文件目录：\n{json.dumps(catalog, ensure_ascii=False)}"
            ),
        },
        {"role": "user", "content": question},
    ]

class TaskCancelled(Exception):
    pass

def check_task_cancel(cancel_event: Optional[threading.Event]) -> None:
    if cancel_event and cancel_event.is_set():
        raise TaskCancelled("任务已取消")

def get_markitdown_converter():
    global markitdown_converter
    if markitdown_converter is not None:
        return markitdown_converter

    if MarkItDown is None:
        raise HTTPException(status_code=503, detail=f"markitdown 加载失败: {MARKITDOWN_IMPORT_ERROR}")

    markitdown_converter = MarkItDown()
    return markitdown_converter

markdown_service = MarkdownService(SUPPORTED_SUMMARY_EXTENSIONS, get_markitdown_converter)

def require_bearer_token(authorization: Optional[str]) -> str:
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="Missing SmartNAS bearer token")
    return authorization

def split_text(value: str, max_chars: int) -> List[str]:
    chunks = []
    current = []
    current_len = 0

    for paragraph in value.splitlines():
        if len(paragraph) > max_chars:
            if current:
                chunks.append("\n".join(current).strip())
                current = []
                current_len = 0
            chunks.extend(paragraph[i:i + max_chars] for i in range(0, len(paragraph), max_chars))
            continue
        addition = len(paragraph) + 1
        if current and current_len + addition > max_chars:
            chunks.append("\n".join(current).strip())
            current = []
            current_len = 0
        current.append(paragraph)
        current_len += addition

    if current:
        chunks.append("\n".join(current).strip())

    return [chunk for chunk in chunks if chunk]

def answer_with_rag(question: str, results: List[Dict[str, Any]], history: Optional[List[dict]] = None) -> str:
    return create_chat_completion(build_rag_messages(question, results, history))

def build_rag_messages(question: str, results: List[Dict[str, Any]], history: Optional[List[dict]] = None) -> List[dict]:
    context = "\n\n".join(
        (
            f"[来源 {i + 1}] 文件: {item.get('filename')} "
            f"片段: {item.get('chunk_index')} 相似度: {item.get('score')}\n"
            f"{item.get('text')}"
        )
        for i, item in enumerate(results)
    )
    return [
        {
            "role": "system",
            "content": (
                "你是 SmartNAS 的 RAG 文件问答助手。只能根据提供的来源片段回答。"
                "如果片段无法支持答案，请明确说没有在已索引文件中找到依据。"
                "回答末尾用“来源：文件名”列出相关文件。"
            ),
        },
        *(history or [])[-6:],
        {
            "role": "user",
            "content": f"用户问题：{question}\n\n检索到的来源片段：\n{context}",
        },
    ]

def execute_search_files_tool(token: str, keyword: str) -> str:
    try:
        owner = get_user_identity(token)
    except Exception:
        owner = "unknown"
    try:
        response = requests.get(
            f"{NAS_CORE_API}/api/v1/files/search",
            params={"keyword": keyword},
            headers={"Authorization": token},
            timeout=10,
        )
    except requests.RequestException as exc:
        result = json.dumps({"error": f"无法连接核心服务: {exc}"}, ensure_ascii=False)
        audit_event(owner, "tool_search_files", "failed", keyword=keyword, result=result)
        return result
    if response.status_code != 200:
        result = json.dumps({"error": f"核心 API 返回 {response.status_code}"}, ensure_ascii=False)
        audit_event(owner, "tool_search_files", "failed", keyword=keyword, result=result)
        return result
    result = json.dumps({"keyword": keyword, "files": response.json()}, ensure_ascii=False)
    audit_event(owner, "tool_search_files", "success", keyword=keyword, result=result)
    return result

def append_tool_results(history: List[dict], assistant_message: Dict[str, Any], token: str) -> bool:
    tool_calls = assistant_message.get("tool_calls") or []
    if not tool_calls:
        return False
    history.append(assistant_message)
    for call in tool_calls:
        function = call.get("function") or {}
        name = function.get("name")
        try:
            arguments = json.loads(function.get("arguments") or "{}")
        except ValueError:
            arguments = {}
        if name == "search_files":
            content = execute_search_files_tool(token, str(arguments.get("keyword") or "").strip())
        else:
            content = json.dumps({"error": f"未知工具: {name}"}, ensure_ascii=False)
        history.append({"role": "tool", "tool_call_id": call.get("id"), "name": name, "content": content})
    return True

def compact_chat_history(history: List[dict]) -> List[dict]:
    if not history:
        return [{"role": "system", "content": system_prompt}]
    system = history[0] if history[0].get("role") == "system" else {"role": "system", "content": system_prompt}
    conversational = [
        message for message in history[1:]
        if message.get("role") in {"user", "assistant"} and message.get("content")
    ]
    return [system] + conversational[-6:]

def summarize_markdown_chunk(markdown_text: str, title: str = "") -> str:
    heading = f"文件名：{title}\n\n" if title else ""
    messages = [
        {
            "role": "system",
            "content": "你是 SmartNAS 的文件摘要助手。请基于 Markdown 内容生成中文摘要，用于网盘搜索和快速预览。",
        },
        {
            "role": "user",
            "content": (
                "请总结下面文件片段，要求：\n"
                "1. 提炼主题、关键事实、日期、金额、项目名、结论；\n"
                "2. 忽略页眉页脚和重复噪声；\n"
                "3. 控制在 180 字以内。\n\n"
                f"{heading}Markdown 内容：\n{markdown_text}"
            ),
        },
    ]
    return create_chat_completion(messages)

def build_summary(markdown_text: str, title: str = "") -> str:
    if not markdown_text.strip():
        raise HTTPException(status_code=422, detail="文档转换后没有可摘要的文本内容")

    chunks = split_text(markdown_text[:MAX_MARKDOWN_CHARS * 4], SUMMARY_CHUNK_CHARS)
    if not chunks:
        raise HTTPException(status_code=422, detail="文档转换后没有可摘要的文本内容")

    if len(chunks) == 1:
        clipped = chunks[0]
        heading = f"文件名：{title}\n\n" if title else ""
        messages = [
            {
                "role": "system",
                "content": "你是 SmartNAS 的文件摘要助手。请基于 Markdown 内容生成中文摘要，用于网盘搜索和快速预览。",
            },
            {
                "role": "user",
                "content": (
                    "请总结下面文件内容，要求：\n"
                    "1. 先用一句话说明主题；\n"
                    "2. 提炼 3 到 6 个关键点；\n"
                    "3. 保留重要人名、日期、金额、项目名、结论；\n"
                    "4. 控制在 300 字以内。\n\n"
                    f"{heading}Markdown 内容：\n{clipped}"
                ),
            },
        ]
        return create_chat_completion(messages)

    partial_summaries = [summarize_markdown_chunk(chunk, title) for chunk in chunks]
    messages = [
        {
            "role": "system",
            "content": "你是 SmartNAS 的文件摘要合并助手。请把多个片段摘要合成为一个可靠的中文文件摘要。",
        },
        {
            "role": "user",
            "content": (
                "请合并下面这些片段摘要，要求：\n"
                "1. 先用一句话说明主题；\n"
                "2. 提炼 3 到 6 个关键点；\n"
                "3. 保留重要人名、日期、金额、项目名、结论；\n"
                "4. 控制在 300 字以内。\n\n"
                f"文件名：{title}\n\n片段摘要：\n" + "\n\n".join(f"{i + 1}. {summary}" for i, summary in enumerate(partial_summaries))
            ),
        },
    ]
    return create_chat_completion(messages)

def build_tags(summary: str, title: str = "") -> List[str]:
    messages = [
        {
            "role": "system",
            "content": "你是 SmartNAS 的文件分类助手。请为文件生成简洁、稳定、适合搜索与分类的中文标签。",
        },
        {
            "role": "user",
            "content": (
                "根据文件名和摘要生成 3 到 8 个标签，兼顾文件类型、主题领域和关键对象。"
                "只返回 JSON 字符串数组，不要 Markdown，不要解释。\n\n"
                f"文件名：{title}\n摘要：{summary}"
            ),
        },
    ]
    raw = create_chat_completion(messages).strip()
    fenced = re.fullmatch(r"```(?:json)?\s*(.*?)\s*```", raw, re.DOTALL | re.IGNORECASE)
    if fenced:
        raw = fenced.group(1)
    try:
        values = json.loads(raw)
    except (TypeError, ValueError):
        match = re.search(r"\[[\s\S]*\]", raw)
        if not match:
            raise HTTPException(status_code=502, detail="标签模型未返回有效 JSON 数组")
        try:
            values = json.loads(match.group(0))
        except (TypeError, ValueError) as exc:
            raise HTTPException(status_code=502, detail=f"标签 JSON 解析失败: {exc}")

    if not isinstance(values, list):
        raise HTTPException(status_code=502, detail="标签模型返回值不是 JSON 数组")

    tags = []
    seen = set()
    for value in values:
        if not isinstance(value, str):
            continue
        tag = re.sub(r"\s+", " ", value).strip(" #,，")[:24]
        key = tag.casefold()
        if tag and key not in seen:
            seen.add(key)
            tags.append(tag)
        if len(tags) == 8:
            break
    if not tags:
        raise HTTPException(status_code=502, detail="标签模型没有返回可用标签")
    return tags

def convert_file_to_markdown(file_hash: str, token: str, force: bool = False):
    return markdown_service.convert(file_hash, token, force)

def update_core_summary(file_hash: str, token: str, summary: str):
    try:
        update = requests.post(
            f"{NAS_CORE_API}/api/v1/files/summary",
            headers={"Authorization": token, "File-Hash": file_hash},
            data=summary.encode("utf-8"),
            timeout=20,
        )
    except Exception as exc:
        raise HTTPException(status_code=502, detail=f"摘要写回核心服务失败: {exc}")

    if update.status_code != 200:
        raise HTTPException(status_code=update.status_code, detail=f"摘要写回失败: {update.text}")

def update_core_tags(file_hash: str, token: str, tags: List[str]):
    try:
        update = requests.post(
            f"{NAS_CORE_API}/api/v1/files/tags",
            headers={"Authorization": token, "File-Hash": file_hash},
            data=json.dumps(tags, ensure_ascii=False).encode("utf-8"),
            timeout=20,
        )
    except Exception as exc:
        raise HTTPException(status_code=502, detail=f"标签写回核心服务失败: {exc}")

    if update.status_code != 200:
        raise HTTPException(status_code=update.status_code, detail=f"标签写回失败: {update.text}")

def run_summary(file_hash: str, token: str, force: bool = False, cancel_event: Optional[threading.Event] = None):
    check_task_cancel(cancel_event)
    filename, markdown_text, cache_hit = convert_file_to_markdown(file_hash, token, force)
    check_task_cancel(cancel_event)
    summary = build_summary(markdown_text, filename)
    check_task_cancel(cancel_event)
    tags = build_tags(summary, filename)
    check_task_cancel(cancel_event)
    update_core_summary(file_hash, token, summary)
    update_core_tags(file_hash, token, tags)
    check_task_cancel(cancel_event)
    rag_status = {"indexed": False, "chunks": 0, "error": None}
    try:
        rag_status["chunks"] = index_markdown_for_rag(file_hash, filename, markdown_text, token)
        rag_status["indexed"] = rag_status["chunks"] > 0
        if rag_status["indexed"]:
            rag_status["keyword"] = rebuild_keyword_index(token)
    except Exception as exc:
        rag_status["error"] = str(exc.detail) if isinstance(exc, HTTPException) else str(exc)
    check_task_cancel(cancel_event)
    return {
        "status": "success",
        "filename": filename,
        "hash": file_hash,
        "markdown_chars": len(markdown_text),
        "cache_hit": cache_hit,
        "summary": summary,
        "tags": tags,
        "rag": rag_status,
    }

def init_task_store() -> None:
    tasks = task_store.initialize()
    summary_tasks.update({task["id"]: task for task in tasks["summary"]})
    index_tasks.update({task["id"]: task for task in tasks["index"]})

def persist_summary_task(task: Dict[str, Any]) -> None:
    task_store.save(task, "summary")

def persist_index_task(task: Dict[str, Any]) -> None:
    task_store.save(task, "index")

def set_summary_task(task_id: str, **updates):
    with summary_tasks_lock:
        if task_id in summary_tasks:
            summary_tasks[task_id].update(updates)
            summary_tasks[task_id]["updated_at"] = time.time()
            persist_summary_task(summary_tasks[task_id])

def summary_task_worker(task_id: str, file_hash: str, token: str, force: bool):
    cancel_event = task_cancel_events.setdefault(task_id, threading.Event())
    owner = summary_tasks.get(task_id, {}).get("owner", "unknown")
    started_at = time.time()
    set_summary_task(task_id, status="running", message="正在生成摘要和标签")
    audit_event(owner, "summary_task", "running", task_id=task_id, file_hash=file_hash, force=force)
    try:
        result = run_summary(file_hash, token, force, cancel_event)
        check_task_cancel(cancel_event)
        set_summary_task(task_id, status="success", message="摘要和标签已生成", result=result)
        audit_event(
            owner, "summary_task", "success", task_id=task_id, file_hash=file_hash,
            duration_ms=round((time.time() - started_at) * 1000), result=result,
        )
    except TaskCancelled:
        set_summary_task(task_id, status="cancelled", message="任务已取消")
        audit_event(owner, "summary_task", "cancelled", task_id=task_id, file_hash=file_hash)
    except HTTPException as exc:
        set_summary_task(task_id, status="failed", message=str(exc.detail), error=str(exc.detail))
        audit_event(
            owner, "summary_task", "failed", task_id=task_id, file_hash=file_hash,
            duration_ms=round((time.time() - started_at) * 1000), error=str(exc.detail),
        )
    except Exception as exc:
        set_summary_task(task_id, status="failed", message=f"摘要和标签任务异常: {exc}", error=str(exc))
        audit_event(
            owner, "summary_task", "failed", task_id=task_id, file_hash=file_hash,
            duration_ms=round((time.time() - started_at) * 1000),
            error=str(exc), exception_type=type(exc).__name__,
        )
    finally:
        task_cancel_events.pop(task_id, None)

def enqueue_summary(file_hash: str, token: str, force: bool = False):
    owner = get_user_identity(token)
    task_id = uuid.uuid4().hex
    now = time.time()
    with summary_tasks_lock:
        duplicate = next(
            (
                task for task in summary_tasks.values()
                if task.get("owner") == owner
                and task.get("hash") == file_hash
                and task.get("status") in {"pending", "running", "cancel_requested"}
            ),
            None,
        )
        if duplicate:
            audit_event(
                owner, "summary_enqueue", "deduplicated", file_hash=file_hash,
                existing_task_id=duplicate.get("id"), existing_status=duplicate.get("status"),
            )
            return duplicate
        summary_tasks[task_id] = {
            "id": task_id,
            "owner": owner,
            "hash": file_hash,
            "force": force,
            "status": "pending",
            "message": "等待摘要和标签任务执行",
            "created_at": now,
            "updated_at": now,
        }
        persist_summary_task(summary_tasks[task_id])
        task_cancel_events[task_id] = threading.Event()
        audit_event(owner, "summary_enqueue", "queued", task=summary_tasks[task_id])
    summary_executor.submit(summary_task_worker, task_id, file_hash, token, force)
    return summary_tasks[task_id]

def find_missing_index_files(token: str) -> Dict[str, Any]:
    return compute_missing_index_files(fetch_all_user_files(token), rag_index_status(token), SUPPORTED_SUMMARY_EXTENSIONS)

def rebuild_index_for_file(file_hash: str, token: str, force: bool = False) -> Dict[str, Any]:
    filename, markdown_text, cache_hit = convert_file_to_markdown(file_hash, token, force)
    chunks = index_markdown_for_rag(file_hash, filename, markdown_text, token)
    mark_keyword_index_dirty(token, "file index rebuilt")
    return {
        "hash": file_hash,
        "filename": filename,
        "markdown_chars": len(markdown_text),
        "cache_hit": cache_hit,
        "chunks": chunks,
        "indexed": chunks > 0,
    }

def rebuild_indexes(token: str, force: bool = False, file_hash: Optional[str] = None, include_keyword: bool = True) -> Dict[str, Any]:
    targets = []
    skipped_unsupported = []
    if file_hash:
        targets.append({"hash": file_hash})
    elif not force:
        missing = find_missing_index_files(token)
        targets = missing["missing"]
        skipped_unsupported = missing["skipped_unsupported"]
    else:
        for file_info in fetch_all_user_files(token):
            current_hash = file_info.get("hash", "")
            name = file_info.get("name", "")
            if not current_hash:
                continue
            if Path(name).suffix.lower() not in SUPPORTED_SUMMARY_EXTENSIONS:
                skipped_unsupported.append({"hash": current_hash, "name": name})
                continue
            targets.append({"hash": current_hash, "name": name})

    indexed = []
    failed = []
    for target in targets:
        current_hash = target.get("hash", "")
        if not current_hash:
            continue
        try:
            indexed.append(rebuild_index_for_file(current_hash, token, force))
        except Exception as exc:
            detail = exc.detail if isinstance(exc, HTTPException) else str(exc)
            failed.append({"hash": current_hash, "name": target.get("name", ""), "error": str(detail)})

    keyword = None
    if include_keyword:
        try:
            keyword = rebuild_keyword_index(token)
        except Exception as exc:
            keyword = {"error": str(exc.detail) if isinstance(exc, HTTPException) else str(exc)}

    return {
        "status": "success" if not failed else "partial_success",
        "target_count": len(targets),
        "indexed_count": len(indexed),
        "failed_count": len(failed),
        "indexed": indexed,
        "failed": failed,
        "skipped_unsupported": skipped_unsupported,
        "keyword": keyword,
    }

def set_index_task(task_id: str, **updates):
    with index_tasks_lock:
        if task_id in index_tasks:
            index_tasks[task_id].update(updates)
            index_tasks[task_id]["updated_at"] = time.time()
            persist_index_task(index_tasks[task_id])

def index_task_worker(task_id: str, token: str, owner: str, force: bool, file_hash: Optional[str], include_keyword: bool):
    started_at = time.time()
    set_index_task(task_id, status="running", message="正在重建索引")
    audit_event(owner, "index_rebuild_task", "running", task_id=task_id, file_hash=file_hash, force=force)
    try:
        result = rebuild_indexes(token, force=force, file_hash=file_hash, include_keyword=include_keyword)
        set_index_task(task_id, status=result["status"], message="索引重建完成", result=result)
        audit_event(
            owner,
            "index_rebuild_task",
            result["status"],
            task_id=task_id,
            duration_ms=round((time.time() - started_at) * 1000),
            result={**result, "indexed": f"{len(result.get('indexed', []))} files"},
        )
    except Exception as exc:
        detail = exc.detail if isinstance(exc, HTTPException) else str(exc)
        set_index_task(task_id, status="failed", message=f"索引重建失败: {detail}", error=str(detail))
        audit_event(
            owner,
            "index_rebuild_task",
            "failed",
            task_id=task_id,
            duration_ms=round((time.time() - started_at) * 1000),
            error=str(detail),
        )

def enqueue_index_rebuild(token: str, owner: str, force: bool = False, file_hash: Optional[str] = None, include_keyword: bool = True):
    task_id = uuid.uuid4().hex
    now = time.time()
    with index_tasks_lock:
        duplicate = next(
            (
                task for task in index_tasks.values()
                if task.get("owner") == owner
                and task.get("hash") == (file_hash or "")
                and task.get("status") in {"pending", "running"}
            ),
            None,
        )
        if duplicate:
            audit_event(owner, "index_rebuild_enqueue", "deduplicated", existing_task_id=duplicate.get("id"))
            return duplicate
        index_tasks[task_id] = {
            "id": task_id,
            "owner": owner,
            "hash": file_hash or "",
            "force": force,
            "include_keyword": include_keyword,
            "status": "pending",
            "message": "等待索引重建任务执行",
            "created_at": now,
            "updated_at": now,
        }
        persist_index_task(index_tasks[task_id])
        audit_event(owner, "index_rebuild_enqueue", "queued", task=index_tasks[task_id])
    summary_executor.submit(index_task_worker, task_id, token, owner, force, file_hash, include_keyword)
    return index_tasks[task_id]

init_task_store()

@app.get("/api/agent/health")
async def health():
    return {
        "status": "ok",
        "core_api": NAS_CORE_API,
        "llm_provider": "deepseek",
        "deepseek_api_base": DEEPSEEK_API_BASE,
        "deepseek_model": DEEPSEEK_MODEL,
        "deepseek_api_key_configured": bool(DEEPSEEK_API_KEY),
        "temperature": GENERATION_TEMPERATURE,
        "top_p": GENERATION_TOP_P,
        "task_db": str(TASK_DB_PATH),
        "file_qa_direct_chars": FILE_QA_DIRECT_CHARS,
        "audit_log": {
            "path": str(AUDIT_LOG_PATH),
            "max_bytes": AUDIT_LOG_MAX_BYTES,
            "backups": AUDIT_LOG_BACKUPS,
            "full_content": AUDIT_LOG_FULL_CONTENT,
        },
        "markitdown_available": MarkItDown is not None,
        "markitdown_error": str(MARKITDOWN_IMPORT_ERROR) if MARKITDOWN_IMPORT_ERROR else None,
        "rag": rag_health_status(),
        "summary_extensions": sorted(SUPPORTED_SUMMARY_EXTENSIONS),
    }

@app.post("/api/agent/summarize")
async def summarize_endpoint(request: SummarizeRequest, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    file_hash = request.hash.strip()
    if not file_hash:
        raise HTTPException(status_code=400, detail="Missing file hash")

    started_at = time.time()
    try:
        result = run_summary(file_hash, token, request.force)
        audit_event(owner, "summarize_sync", "success", file_hash=file_hash, force=request.force,
                    duration_ms=round((time.time() - started_at) * 1000), result=result)
        return result
    except Exception as exc:
        detail = exc.detail if isinstance(exc, HTTPException) else str(exc)
        audit_event(owner, "summarize_sync", "failed", file_hash=file_hash, force=request.force,
                    duration_ms=round((time.time() - started_at) * 1000), error=str(detail))
        raise

@app.post("/api/agent/summarize/start")
async def start_summarize_endpoint(request: SummarizeRequest, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    file_hash = request.hash.strip()
    if not file_hash:
        raise HTTPException(status_code=400, detail="Missing file hash")
    task = enqueue_summary(file_hash, token, request.force)
    audit_event(owner, "summarize_start", "success", file_hash=file_hash, force=request.force, task=task)
    return task

@app.get("/api/agent/summarize/status/{task_id}")
async def summarize_status_endpoint(task_id: str, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    with summary_tasks_lock:
        task = summary_tasks.get(task_id)
        if not task or task.get("owner") != owner:
            audit_event(owner, "summary_status", "not_found", task_id=task_id)
            raise HTTPException(status_code=404, detail="摘要任务不存在")
        audit_event(owner, "summary_status", "success", task=task)
        return task

@app.get("/api/agent/summarize/tasks")
async def summarize_tasks_endpoint(authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    with summary_tasks_lock:
        tasks = [task for task in summary_tasks.values() if task.get("owner") == owner]
        result = {"tasks": tasks[-100:]}
        audit_event(owner, "summary_tasks_list", "success", result=result)
        return result

@app.post("/api/agent/summarize/cancel/{task_id}")
async def cancel_summarize_endpoint(task_id: str, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    with summary_tasks_lock:
        task = summary_tasks.get(task_id)
        if not task or task.get("owner") != owner:
            audit_event(owner, "summary_cancel", "not_found", task_id=task_id)
            raise HTTPException(status_code=404, detail="摘要任务不存在")
        if task.get("status") not in {"pending", "running", "cancel_requested"}:
            audit_event(owner, "summary_cancel", "ignored", task=task)
            return task
        event = task_cancel_events.setdefault(task_id, threading.Event())
        event.set()
        task.update(status="cancel_requested", message="正在取消任务", updated_at=time.time())
        persist_summary_task(task)
        audit_event(owner, "summary_cancel", "requested", task=task)
        return task

@app.post("/api/agent/summarize/retry/{task_id}")
async def retry_summarize_endpoint(task_id: str, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    with summary_tasks_lock:
        task = summary_tasks.get(task_id)
        if not task or task.get("owner") != owner:
            audit_event(owner, "summary_retry", "not_found", task_id=task_id)
            raise HTTPException(status_code=404, detail="摘要任务不存在")
        if task.get("status") not in {"failed", "cancelled"}:
            audit_event(owner, "summary_retry", "rejected", task=task)
            raise HTTPException(status_code=409, detail="只有失败或已取消任务可以重试")
        file_hash = task["hash"]
        force = bool(task.get("force"))
    retried = enqueue_summary(file_hash, token, force)
    audit_event(owner, "summary_retry", "success", previous_task_id=task_id, task=retried)
    return retried

@app.post("/api/agent/summarize/missing")
async def summarize_missing_endpoint(authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    tasks = []
    skipped_existing = []
    skipped_unsupported = []
    file_items = fetch_all_user_files(token)
    for file_info in file_items:
        name = file_info.get("name", "")
        file_hash = file_info.get("hash", "")
        summary = (file_info.get("summary") or "").strip()
        if not file_hash:
            continue
        if Path(name).suffix.lower() not in SUPPORTED_SUMMARY_EXTENSIONS:
            skipped_unsupported.append({"hash": file_hash, "name": name})
            continue
        if summary:
            skipped_existing.append({"hash": file_hash, "name": name})
            continue
        tasks.append(enqueue_summary(file_hash, token, False))

    result = {
        "status": "success",
        "scanned": len(file_items),
        "queued": len(tasks),
        "skipped_existing_summary": len(skipped_existing),
        "skipped_unsupported": len(skipped_unsupported),
        "tasks": tasks,
    }
    audit_event(
        owner,
        "summarize_missing",
        "success",
        result=result,
        queued_hashes=[task.get("hash") for task in tasks],
        skipped_existing=skipped_existing,
        skipped_unsupported=skipped_unsupported,
    )
    return result

@app.get("/api/agent/markdown/{file_hash}")
async def markdown_endpoint(file_hash: str, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    started_at = time.time()
    filename, markdown_text, cache_hit = convert_file_to_markdown(file_hash, token, False)
    result = {
        "status": "success",
        "filename": filename,
        "hash": file_hash,
        "cache_hit": cache_hit,
        "markdown": markdown_text,
    }
    audit_event(owner, "markdown_read", "success", file_hash=file_hash,
                duration_ms=round((time.time() - started_at) * 1000),
                result={**result, "markdown": audit_content(markdown_text)})
    return result

def prepare_file_qa_context(owner: str, token: str, file_hash: str, question: str) -> Dict[str, Any]:
    session_key = f"{owner}:{file_hash}"
    history = file_qa_sessions.setdefault(session_key, [])
    filename, markdown_text, _ = convert_file_to_markdown(file_hash, token, False)
    mode = "full"
    fallback_answer = ""
    sources: List[Dict[str, Any]] = []

    if len(markdown_text) > FILE_QA_DIRECT_CHARS:
        mode = "rag"
        if not rag_has_file_index(token, file_hash):
            chunks = index_markdown_for_rag(file_hash, filename, markdown_text, token)
            if chunks > 0:
                mark_keyword_index_dirty(token, "file qa indexed missing chunks")
        previous_question = next(
            (item.get("content", "") for item in reversed(history) if item.get("role") == "user"),
            "",
        )
        retrieval_query = f"{previous_question}\n{question}".strip()
        search = search_documents(
            retrieval_query,
            token,
            max(RAG_TOP_K, 8),
            filters=RetrievalFilters(file_hash=file_hash),
        )
        if search.get("results"):
            messages = build_rag_messages(question, search["results"], history)
            sources = [
                {"chunk_index": item.get("chunk_index"), "score": item.get("score")}
                for item in search["results"]
            ]
        else:
            messages = None
            fallback_answer = "没有在该文件的相关片段中找到足够依据。"
    else:
        messages = [
            {
                "role": "system",
                "content": (
                    "你是 SmartNAS 的文件问答助手。只能根据提供的文件内容回答；"
                    "如果内容里没有答案，请明确说未在文件中找到。"
                    f"\n\n文件名：{filename}\n\n文件内容：\n{markdown_text}"
                ),
            },
            *history[-6:],
            {"role": "user", "content": question},
        ]

    return {
        "session_key": session_key,
        "history": history,
        "filename": filename,
        "mode": mode,
        "messages": messages,
        "fallback_answer": fallback_answer,
        "sources": sources,
    }

def record_file_qa_answer(context: Dict[str, Any], question: str, answer: str) -> None:
    history = context["history"]
    history.extend([
        {"role": "user", "content": question},
        {"role": "assistant", "content": answer},
    ])
    file_qa_sessions[context["session_key"]] = history[-6:]

@app.post("/api/agent/file_qa")
async def file_qa_endpoint(request: FileQuestionRequest, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    started_at = time.time()
    file_hash = request.hash.strip()
    question = request.question.strip()
    if not file_hash or not question:
        raise HTTPException(status_code=400, detail="Missing hash or question")

    context = prepare_file_qa_context(owner, token, file_hash, question)
    answer = context["fallback_answer"] or create_chat_completion(context["messages"])
    record_file_qa_answer(context, question, answer)
    result = {
        "status": "success",
        "filename": context["filename"],
        "hash": file_hash,
        "mode": context["mode"],
        "answer": answer,
        "sources": context["sources"],
    }
    audit_event(owner, "file_qa", "success", file_hash=file_hash, question=question,
                duration_ms=round((time.time() - started_at) * 1000), result=result)
    return result

@app.post("/api/agent/file_qa/stream")
async def file_qa_stream_endpoint(request: FileQuestionRequest, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    started_at = time.time()
    file_hash = request.hash.strip()
    question = request.question.strip()
    if not file_hash or not question:
        raise HTTPException(status_code=400, detail="Missing hash or question")

    context = prepare_file_qa_context(owner, token, file_hash, question)

    def event_stream():
        answer_parts: List[str] = []
        audit_event(owner, "file_qa_stream", "started", file_hash=file_hash,
                    question=question, mode=context["mode"])
        try:
            if context["fallback_answer"]:
                answer_parts.append(context["fallback_answer"])
                yield sse_event("delta", content=context["fallback_answer"])
            elif context["messages"] is not None:
                for content in stream_chat_completion(context["messages"]):
                    answer_parts.append(content)
                    yield sse_event("delta", content=content)

            answer = "".join(answer_parts)
            record_file_qa_answer(context, question, answer)
            audit_event(owner, "file_qa_stream", "success", file_hash=file_hash, question=question,
                        mode=context["mode"], duration_ms=round((time.time() - started_at) * 1000), answer=answer)
            yield sse_event("done", mode=context["mode"])
        except GeneratorExit:
            audit_event(owner, "file_qa_stream", "cancelled", file_hash=file_hash, question=question,
                        mode=context["mode"], partial_answer="".join(answer_parts))
            raise
        except Exception as exc:
            detail = exc.detail if isinstance(exc, HTTPException) else str(exc)
            audit_event(owner, "file_qa_stream", "failed", file_hash=file_hash, question=question,
                        mode=context["mode"], partial_answer="".join(answer_parts), error=str(detail))
            yield sse_event("error", message=str(detail))

    return sse_response(event_stream())

@app.post("/api/agent/rag/query")
async def rag_query_endpoint(request: RagQueryRequest, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    started_at = time.time()
    query = request.query.strip()
    if not query:
        raise HTTPException(status_code=400, detail="Missing query")
    try:
        file_hash_filter = (request.file_hash or request.hash or "").strip() or None
        search = search_documents(
            query,
            token,
            request.top_k,
            filters=RetrievalFilters(
                file_hash=file_hash_filter,
                directory=(request.directory or "").strip() or None,
            ),
        )
    except HTTPException:
        raise
    except Exception as exc:
        raise HTTPException(status_code=503, detail=f"RAG 检索失败: {exc}")

    answer = ""
    if search.get("results"):
        answer = answer_with_rag(query, search["results"])
    result = {"status": "success", "query": query, "answer": answer, **search}
    audit_event(owner, "rag_query", "success", duration_ms=round((time.time() - started_at) * 1000), result=result)
    return result

@app.get("/api/agent/index/status")
async def index_status_endpoint(authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    result = rag_index_status(token)
    result["keyword"] = keyword_index_status(token)
    audit_event(owner, "index_status", "success", result={**result, "files": f"{len(result.get('files', []))} files"})
    return {"status": "success", **result}

@app.get("/api/agent/index/missing")
async def index_missing_endpoint(authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    result = find_missing_index_files(token)
    audit_event(owner, "index_missing", "success", result={**result, "missing": f"{len(result.get('missing', []))} files"})
    return {"status": "success", **result}

@app.post("/api/agent/index/rebuild/start")
async def start_index_rebuild_endpoint(request: IndexRebuildRequest, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    file_hash = (request.hash or "").strip() or None
    task = enqueue_index_rebuild(token, owner, request.force, file_hash, request.include_keyword)
    audit_event(owner, "index_rebuild_start", "success", task=task)
    return task

@app.get("/api/agent/index/rebuild/status/{task_id}")
async def index_rebuild_status_endpoint(task_id: str, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    with index_tasks_lock:
        task = index_tasks.get(task_id)
        if not task or task.get("owner") != owner:
            audit_event(owner, "index_rebuild_status", "not_found", task_id=task_id)
            raise HTTPException(status_code=404, detail="索引任务不存在")
        audit_event(owner, "index_rebuild_status", "success", task=task)
        return task

@app.get("/api/agent/index/rebuild/tasks")
async def index_rebuild_tasks_endpoint(authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    with index_tasks_lock:
        tasks = [task for task in index_tasks.values() if task.get("owner") == owner]
        result = {"tasks": tasks[-100:]}
        audit_event(owner, "index_rebuild_tasks_list", "success", result=result)
        return result

@app.post("/api/agent/chat")
async def chat_endpoint(request: ChatRequest, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    prompt = request.prompt.strip()
    started_at = time.time()
    if not prompt:
        raise HTTPException(status_code=400, detail="Missing prompt")

    history = sessions.setdefault(owner, [])

    if not history:
        history.append({"role": "system", "content": system_prompt})

    if len(history) > 10:
        sessions[owner] = compact_chat_history(history)
        history = sessions[owner]

    history.append({"role": "user", "content": prompt})

    try:
        if choose_query_route(prompt) == "catalog":
            answer = create_chat_completion(build_file_catalog_messages(prompt, token))
            history.append({"role": "assistant", "content": answer})
            result = {"status": "success", "mode": "catalog", "response": answer}
            audit_event(owner, "chat", "success", prompt=prompt,
                        duration_ms=round((time.time() - started_at) * 1000), result=result)
            return result

        try:
            rag_search = search_documents(prompt, token, RAG_TOP_K)
        except Exception as exc:
            print(f"[RAG] Unavailable, fallback to summary search: {exc}")
            rag_search = {"available": False, "results": [], "reason": str(exc)}

        if rag_search.get("results"):
            answer = answer_with_rag(prompt, rag_search["results"])
            history.append({"role": "assistant", "content": answer})
            result = {
                "status": "success",
                "mode": "rag",
                "response": answer,
                "sources": [
                    {
                        "filename": item.get("filename"),
                        "hash": item.get("hash"),
                        "chunk_index": item.get("chunk_index"),
                        "score": item.get("score"),
                    }
                    for item in rag_search["results"]
                ],
            }
            audit_event(owner, "chat", "success", prompt=prompt,
                        duration_ms=round((time.time() - started_at) * 1000), result=result)
            return result

        assistant_message = create_chat_message(history, SEARCH_FILE_TOOLS)
        if append_tool_results(history, assistant_message, token):
            final_text = create_chat_completion(history)
            history.append({"role": "assistant", "content": final_text})
            result = {"status": "success", "mode": "tool", "response": final_text}
            audit_event(owner, "chat", "success", prompt=prompt,
                        duration_ms=round((time.time() - started_at) * 1000), result=result)
            return result

        text = (assistant_message.get("content") or "").strip()
        history.append({"role": "assistant", "content": text})
        result = {"status": "success", "mode": "chat", "response": text}
        audit_event(owner, "chat", "success", prompt=prompt,
                    duration_ms=round((time.time() - started_at) * 1000), result=result)
        return result
    except HTTPException as exc:
        audit_event(owner, "chat", "failed", prompt=prompt,
                    duration_ms=round((time.time() - started_at) * 1000), error=str(exc.detail))
        raise
    except Exception as e:
        result = {"status": "error", "message": f"Agent 执行异常: {str(e)}"}
        audit_event(owner, "chat", "failed", prompt=prompt,
                    duration_ms=round((time.time() - started_at) * 1000), result=result)
        return result

@app.post("/api/agent/chat/stream")
async def chat_stream_endpoint(request: ChatRequest, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    prompt = request.prompt.strip()
    started_at = time.time()
    if not prompt:
        raise HTTPException(status_code=400, detail="Missing prompt")

    history = sessions.setdefault(owner, [])
    if not history:
        history.append({"role": "system", "content": system_prompt})
    if len(history) > 10:
        sessions[owner] = compact_chat_history(history)
        history = sessions[owner]
    history.append({"role": "user", "content": prompt})

    catalog_messages = None
    try:
        route = choose_query_route(prompt)
    except Exception:
        route = "rag"
    if route == "catalog":
        catalog_messages = build_file_catalog_messages(prompt, token)
        rag_search = {"results": []}
    else:
        try:
            rag_search = search_documents(prompt, token, RAG_TOP_K)
        except Exception:
            rag_search = {"results": []}

    def event_stream():
        answer_parts = []
        mode = "catalog" if catalog_messages else ("rag" if rag_search.get("results") else "chat")
        audit_event(owner, "chat_stream", "started", prompt=prompt, mode=mode)
        try:
            if catalog_messages:
                final_messages = catalog_messages
            elif rag_search.get("results"):
                final_messages = build_rag_messages(prompt, rag_search["results"])
            else:
                # 检索索引为空或暂不可用时，仍先让模型决定是否查询 Core 的
                # 文件名、摘要和标签；否则模型看不到用户的文件清单，容易误报
                # “找不到文件”。
                assistant_message = create_chat_message(history, SEARCH_FILE_TOOLS)
                if append_tool_results(history, assistant_message, token):
                    mode = "tool"
                final_messages = history

            if final_messages is not None:
                for content in stream_chat_completion(final_messages):
                    answer_parts.append(content)
                    yield sse_event("delta", content=content)
            answer = "".join(answer_parts)
            history.append({"role": "assistant", "content": answer})
            audit_event(owner, "chat_stream", "success", prompt=prompt, mode=mode,
                        duration_ms=round((time.time() - started_at) * 1000), answer=answer)
            yield sse_event("done")
        except GeneratorExit:
            audit_event(owner, "chat_stream", "cancelled", prompt=prompt, mode=mode,
                        duration_ms=round((time.time() - started_at) * 1000), partial_answer="".join(answer_parts))
            raise
        except Exception as exc:
            detail = exc.detail if isinstance(exc, HTTPException) else str(exc)
            audit_event(owner, "chat_stream", "failed", prompt=prompt, mode=mode,
                        duration_ms=round((time.time() - started_at) * 1000),
                        partial_answer="".join(answer_parts), error=str(detail))
            yield sse_event("error", message=str(detail))

    return sse_response(event_stream())

@app.post("/api/agent/clear_history")
async def clear_history(authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    sessions.pop(owner, None)
    audit_event(owner, "clear_history", "success")
    return {"status": "success"}
