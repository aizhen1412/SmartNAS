import uvicorn
from fastapi import FastAPI, Header, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import StreamingResponse
from pydantic import BaseModel
import hashlib
import json
import logging
import os
import requests
import re
import shutil
import sqlite3
import tempfile
import threading
import time
import uuid
import io
import mimetypes
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from logging.handlers import RotatingFileHandler
from pathlib import Path
from typing import Dict, List, Optional, Any
from urllib.parse import unquote

from PIL import Image, ExifTags

try:
    import numpy as np
except Exception as exc:
    np = None
    NUMPY_IMPORT_ERROR = exc
else:
    NUMPY_IMPORT_ERROR = None

try:
    import faiss
except Exception as exc:
    faiss = None
    FAISS_IMPORT_ERROR = exc
else:
    FAISS_IMPORT_ERROR = None

try:
    from sentence_transformers import SentenceTransformer
except Exception as exc:
    SentenceTransformer = None
    SENTENCE_TRANSFORMERS_IMPORT_ERROR = exc
else:
    SENTENCE_TRANSFORMERS_IMPORT_ERROR = None

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
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

NAS_CORE_API = os.getenv("SMARTNAS_CORE_API", "http://127.0.0.1:8080")
DEEPSEEK_API_KEY = os.getenv("SMARTNAS_DEEPSEEK_API_KEY") or os.getenv("DEEPSEEK_API_KEY", "")
DEEPSEEK_API_BASE = os.getenv("SMARTNAS_DEEPSEEK_API_BASE", "https://api.deepseek.com").rstrip("/")
DEEPSEEK_MODEL = os.getenv("SMARTNAS_DEEPSEEK_MODEL", "deepseek-chat")
GENERATION_TEMPERATURE = float(os.getenv("SMARTNAS_GENERATION_TEMPERATURE", "0.7"))
GENERATION_TOP_P = float(os.getenv("SMARTNAS_GENERATION_TOP_P", "0.8"))
DEEPSEEK_TIMEOUT = float(os.getenv("SMARTNAS_DEEPSEEK_TIMEOUT", "60"))
MAX_MARKDOWN_CHARS = int(os.getenv("SMARTNAS_MAX_MARKDOWN_CHARS", "12000"))
SUMMARY_CHUNK_CHARS = int(os.getenv("SMARTNAS_SUMMARY_CHUNK_CHARS", "7000"))
CACHE_DIR = Path(os.getenv("SMARTNAS_CACHE_DIR", "var/cache/markdown"))
CACHE_FORMAT_VERSION = 2
VECTOR_DIR = Path(os.getenv("SMARTNAS_VECTOR_DIR", "var/cache/vector"))
EMBEDDING_MODEL_NAME = os.getenv("SMARTNAS_EMBEDDING_MODEL", "BAAI/bge-small-zh-v1.5")
RAG_CHUNK_CHARS = int(os.getenv("SMARTNAS_RAG_CHUNK_CHARS", "900"))
RAG_CHUNK_OVERLAP = int(os.getenv("SMARTNAS_RAG_CHUNK_OVERLAP", "120"))
RAG_TOP_K = int(os.getenv("SMARTNAS_RAG_TOP_K", "6"))
RAG_MIN_SCORE = float(os.getenv("SMARTNAS_RAG_MIN_SCORE", "0.28"))
FILE_QA_DIRECT_CHARS = int(os.getenv("SMARTNAS_FILE_QA_DIRECT_CHARS", "30000"))
TASK_DB_PATH = Path(os.getenv("SMARTNAS_TASK_DB", "var/cache/agent_tasks.db"))
AUDIT_LOG_PATH = Path(os.getenv("SMARTNAS_AUDIT_LOG", "var/log/agent_audit.jsonl"))
AUDIT_LOG_MAX_BYTES = int(os.getenv("SMARTNAS_AUDIT_LOG_MAX_BYTES", str(20 * 1024 * 1024)))
AUDIT_LOG_BACKUPS = int(os.getenv("SMARTNAS_AUDIT_LOG_BACKUPS", "10"))
AUDIT_LOG_FULL_CONTENT = os.getenv("SMARTNAS_AUDIT_LOG_FULL_CONTENT", "1").lower() not in {"0", "false", "no"}
embedding_model = None
markitdown_converter = None
llm_request_lock = threading.Lock()
embedding_generation_lock = threading.Lock()
vector_store_lock = threading.Lock()
summary_executor = ThreadPoolExecutor(max_workers=1)
summary_tasks: Dict[str, Dict[str, Any]] = {}
summary_tasks_lock = threading.Lock()
task_cancel_events: Dict[str, threading.Event] = {}
identity_cache: Dict[str, Dict[str, Any]] = {}
identity_cache_lock = threading.Lock()
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

class ChatRequest(BaseModel):
    prompt: str

class SummarizeRequest(BaseModel):
    hash: str
    force: bool = False

class FileQuestionRequest(BaseModel):
    hash: str
    question: str

class RagQueryRequest(BaseModel):
    query: str
    top_k: int = RAG_TOP_K

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

def get_embedding_model():
    global embedding_model
    if embedding_model is not None:
        return embedding_model

    if SentenceTransformer is None:
        raise HTTPException(status_code=503, detail=f"sentence-transformers 加载失败: {SENTENCE_TRANSFORMERS_IMPORT_ERROR}")

    embedding_model = SentenceTransformer(EMBEDDING_MODEL_NAME)
    return embedding_model

def encode_texts(texts: List[str]):
    if np is None:
        raise HTTPException(status_code=503, detail=f"numpy 加载失败: {NUMPY_IMPORT_ERROR}")
    if not texts:
        return np.zeros((0, 0), dtype="float32")

    model = get_embedding_model()
    with embedding_generation_lock:
        vectors = model.encode(
            texts,
            batch_size=16,
            normalize_embeddings=True,
            show_progress_bar=False,
        )
    return np.asarray(vectors, dtype="float32")

def create_chat_message(messages: List[dict], tools: Optional[List[dict]] = None) -> Dict[str, Any]:
    if not DEEPSEEK_API_KEY:
        raise HTTPException(status_code=503, detail="缺少 DeepSeek API Key，请设置 SMARTNAS_DEEPSEEK_API_KEY 或 DEEPSEEK_API_KEY")

    payload = {
        "model": DEEPSEEK_MODEL,
        "messages": messages,
        "temperature": GENERATION_TEMPERATURE,
        "top_p": GENERATION_TOP_P,
        "stream": False,
    }
    if tools:
        payload["tools"] = tools
        payload["tool_choice"] = "auto"

    try:
        with llm_request_lock:
            response = requests.post(
                f"{DEEPSEEK_API_BASE}/chat/completions",
                headers={
                    "Authorization": f"Bearer {DEEPSEEK_API_KEY}",
                    "Content-Type": "application/json",
                },
                json=payload,
                timeout=DEEPSEEK_TIMEOUT,
            )
    except requests.RequestException as exc:
        raise HTTPException(status_code=502, detail=f"DeepSeek API 请求失败: {exc}")

    if response.status_code != 200:
        detail = response.text
        try:
            detail = response.json().get("error", {}).get("message", detail)
        except ValueError:
            pass
        raise HTTPException(status_code=response.status_code, detail=f"DeepSeek API 返回错误: {detail}")

    try:
        data = response.json()
        return data["choices"][0]["message"]
    except (KeyError, IndexError, TypeError, ValueError) as exc:
        raise HTTPException(status_code=502, detail=f"DeepSeek API 响应格式异常: {exc}")

def create_chat_completion(messages: List[dict]) -> str:
    message = create_chat_message(messages)
    return (message.get("content") or "").strip()

def stream_chat_completion(messages: List[dict]):
    if not DEEPSEEK_API_KEY:
        raise HTTPException(status_code=503, detail="缺少 DeepSeek API Key")
    payload = {
        "model": DEEPSEEK_MODEL,
        "messages": messages,
        "temperature": GENERATION_TEMPERATURE,
        "top_p": GENERATION_TOP_P,
        "stream": True,
    }
    try:
        with requests.post(
            f"{DEEPSEEK_API_BASE}/chat/completions",
            headers={"Authorization": f"Bearer {DEEPSEEK_API_KEY}", "Content-Type": "application/json"},
            json=payload,
            timeout=DEEPSEEK_TIMEOUT,
            stream=True,
        ) as response:
            if response.status_code != 200:
                raise HTTPException(status_code=response.status_code, detail=f"DeepSeek API 返回错误: {response.text}")
            for line in response.iter_lines(decode_unicode=True):
                if not line or not line.startswith("data:"):
                    continue
                data = line[5:].strip()
                if data == "[DONE]":
                    break
                try:
                    payload = json.loads(data)
                    content = payload["choices"][0].get("delta", {}).get("content")
                except (KeyError, IndexError, TypeError, ValueError):
                    continue
                if content:
                    yield content
    except requests.RequestException as exc:
        raise HTTPException(status_code=502, detail=f"DeepSeek 流式请求失败: {exc}")

def require_bearer_token(authorization: Optional[str]) -> str:
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="Missing SmartNAS bearer token")
    return authorization

def get_user_identity(token: str) -> str:
    cache_key = hashlib.sha256(token.encode("utf-8")).hexdigest()
    now = time.time()
    with identity_cache_lock:
        cached = identity_cache.get(cache_key)
        if cached and cached["expires_at"] > now:
            return cached["username"]
    try:
        response = requests.get(
            f"{NAS_CORE_API}/api/v1/me",
            headers={"Authorization": token},
            timeout=10,
        )
    except requests.RequestException as exc:
        raise HTTPException(status_code=502, detail=f"无法验证用户身份: {exc}")
    if response.status_code != 200:
        raise HTTPException(status_code=401, detail="SmartNAS 登录状态无效或已过期")
    username = (response.json().get("username") or "").strip()
    if not username:
        raise HTTPException(status_code=502, detail="核心服务未返回用户身份")
    with identity_cache_lock:
        identity_cache[cache_key] = {"username": username, "expires_at": now + 30}
    return username

def fetch_all_user_files(token: str) -> List[Dict[str, Any]]:
    try:
        response = requests.get(
            f"{NAS_CORE_API}/api/v1/files/all",
            headers={"Authorization": token},
            timeout=20,
        )
    except requests.RequestException as exc:
        raise HTTPException(status_code=502, detail=f"无法读取完整文件清单: {exc}")
    if response.status_code != 200:
        raise HTTPException(status_code=response.status_code, detail="核心服务无法返回完整文件清单")
    payload = response.json()
    return payload.get("files", payload if isinstance(payload, list) else [])

def filename_from_content_disposition(header_value: str) -> str:
    if not header_value:
        return "document"

    match = re.search(r'filename\*?=(?:UTF-8\'\')?"?([^";]+)"?', header_value, re.IGNORECASE)
    if not match:
        return "document"

    return unquote(match.group(1)).strip() or "document"

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

def split_markdown_for_rag(value: str, max_chars: int, overlap: int) -> List[str]:
    chunks = split_text(value, max_chars)
    if overlap <= 0 or len(chunks) < 2:
        return chunks

    overlapped = []
    previous_tail = ""
    for chunk in chunks:
        text = f"{previous_tail}\n{chunk}".strip() if previous_tail else chunk
        overlapped.append(text)
        previous_tail = chunk[-overlap:]
    return overlapped

def vector_namespace(token: str) -> str:
    username = get_user_identity(token)
    return hashlib.sha256(username.encode("utf-8")).hexdigest()[:24]

def vector_namespace_dir(token: str) -> Path:
    path = VECTOR_DIR / vector_namespace(token)
    legacy = VECTOR_DIR / hashlib.sha256(token.encode("utf-8")).hexdigest()[:24]
    if not path.exists() and legacy.exists() and legacy != path:
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(legacy), str(path))
    return path

def vector_chunks_path(token: str) -> Path:
    return vector_namespace_dir(token) / "chunks.json"

def vector_index_path(token: str) -> Path:
    return vector_namespace_dir(token) / "index.faiss"

def load_vector_chunks(token: str) -> List[Dict[str, Any]]:
    path = vector_chunks_path(token)
    if not path.exists():
        return []
    payload = json.loads(path.read_text(encoding="utf-8"))
    if isinstance(payload, dict):
        return payload.get("chunks", [])
    return payload if isinstance(payload, list) else []

def save_vector_chunks(token: str, chunks: List[Dict[str, Any]]) -> None:
    chunks = [chunk for chunk in chunks if chunk.get("embedding") and chunk.get("text")]
    path = vector_chunks_path(token)
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "version": 2,
        "embedding_model": EMBEDDING_MODEL_NAME,
        "updated_at": int(time.time()),
        "chunks": chunks,
    }
    path.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
    index_path = vector_index_path(token)
    if faiss is not None and chunks:
        embeddings = np.asarray([chunk["embedding"] for chunk in chunks], dtype="float32")
        index = faiss.IndexFlatIP(embeddings.shape[1])
        index.add(embeddings)
        temp_path = index_path.with_suffix(".tmp")
        faiss.write_index(index, str(temp_path))
        os.replace(temp_path, index_path)
    elif index_path.exists():
        index_path.unlink()

def rag_has_file_index(token: str, file_hash: str) -> bool:
    with vector_store_lock:
        return any(chunk.get("hash") == file_hash for chunk in load_vector_chunks(token))

def reconcile_vector_store(token: str) -> Dict[str, int]:
    owner = get_user_identity(token)
    files = fetch_all_user_files(token)
    active = {item.get("hash"): item.get("name", "") for item in files if item.get("hash")}
    with vector_store_lock:
        chunks = load_vector_chunks(token)
        kept = []
        removed = 0
        renamed = 0
        for chunk in chunks:
            file_hash = chunk.get("hash")
            if file_hash not in active:
                removed += 1
                continue
            filename = active[file_hash]
            if filename and chunk.get("filename") != filename:
                chunk["filename"] = filename
                renamed += 1
            kept.append(chunk)
        if removed or renamed:
            save_vector_chunks(token, kept)
    result = {"active_files": len(active), "removed_chunks": removed, "renamed_chunks": renamed}
    audit_event(owner, "index_reconcile", "success", result=result)
    return result

def index_markdown_for_rag(file_hash: str, filename: str, markdown_text: str, token: str) -> int:
    raw_chunks = split_markdown_for_rag(markdown_text, RAG_CHUNK_CHARS, RAG_CHUNK_OVERLAP)
    raw_chunks = [chunk for chunk in raw_chunks if chunk.strip()]
    if not raw_chunks:
        return 0

    vectors = encode_texts(raw_chunks)
    now = int(time.time())
    new_chunks = []
    for index, chunk in enumerate(raw_chunks):
        new_chunks.append(
            {
                "id": f"{file_hash}:{index}",
                "hash": file_hash,
                "filename": filename,
                "chunk_index": index,
                "text": chunk,
                "full_document": True,
                "embedding": vectors[index].tolist(),
                "updated_at": now,
            }
        )

    with vector_store_lock:
        existing = [chunk for chunk in load_vector_chunks(token) if chunk.get("hash") != file_hash]
        existing.extend(new_chunks)
        save_vector_chunks(token, existing)
    return len(new_chunks)

def search_rag_chunks(query: str, token: str, top_k: int = RAG_TOP_K, file_hash: Optional[str] = None) -> Dict[str, Any]:
    if not query.strip():
        return {"available": True, "results": [], "reason": "empty query"}

    reconcile_vector_store(token)
    with vector_store_lock:
        chunks = load_vector_chunks(token)
    chunks = [chunk for chunk in chunks if chunk.get("embedding") and chunk.get("text")]
    if file_hash:
        chunks = [chunk for chunk in chunks if chunk.get("hash") == file_hash]
    if not chunks:
        return {"available": True, "results": [], "reason": "empty index"}

    query_vector = encode_texts([query])
    if query_vector.shape[0] == 0:
        return {"available": True, "results": [], "reason": "empty query vector"}

    embeddings = np.asarray([chunk["embedding"] for chunk in chunks], dtype="float32")
    if embeddings.ndim != 2 or embeddings.shape[0] == 0:
        return {"available": True, "results": [], "reason": "empty embeddings"}

    limit = max(1, min(top_k, len(chunks)))
    persisted_index = vector_index_path(token)
    if faiss is not None and not file_hash and persisted_index.exists():
        index = faiss.read_index(str(persisted_index))
        if index.ntotal != len(chunks):
            index = faiss.IndexFlatIP(embeddings.shape[1])
            index.add(embeddings)
        scores, ids = index.search(query_vector, limit)
        pairs = zip(ids[0].tolist(), scores[0].tolist())
    elif faiss is not None:
        index = faiss.IndexFlatIP(embeddings.shape[1])
        index.add(embeddings)
        scores, ids = index.search(query_vector, limit)
        pairs = zip(ids[0].tolist(), scores[0].tolist())
    else:
        scores = embeddings @ query_vector[0]
        order = np.argsort(-scores)[:limit]
        pairs = ((int(i), float(scores[i])) for i in order)

    results = []
    for chunk_index, score in pairs:
        if chunk_index < 0 or score < RAG_MIN_SCORE:
            continue
        chunk = chunks[chunk_index]
        results.append(
            {
                "score": round(float(score), 4),
                "hash": chunk.get("hash"),
                "filename": chunk.get("filename"),
                "chunk_index": chunk.get("chunk_index"),
                "text": chunk.get("text"),
            }
        )
    return {"available": True, "results": results, "reason": None}

def answer_with_rag(question: str, results: List[Dict[str, Any]]) -> str:
    return create_chat_completion(build_rag_messages(question, results))

def build_rag_messages(question: str, results: List[Dict[str, Any]]) -> List[dict]:
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

def markdown_cache_path(file_hash: str) -> Path:
    safe_hash = re.sub(r"[^a-zA-Z0-9_.-]", "_", file_hash)
    return CACHE_DIR / f"{safe_hash}.v{CACHE_FORMAT_VERSION}.md"

def download_file_for_summary(file_hash: str, token: str):
    try:
        download = requests.get(
            f"{NAS_CORE_API}/download",
            params={"hash": file_hash},
            headers={"Authorization": token},
            timeout=60,
        )
    except Exception as exc:
        raise HTTPException(status_code=502, detail=f"无法连接核心下载接口: {exc}")

    if download.status_code == 403:
        raise HTTPException(status_code=403, detail="没有权限读取该文件")
    if download.status_code == 404:
        raise HTTPException(status_code=404, detail="文件不存在")
    if download.status_code != 200:
        raise HTTPException(status_code=download.status_code, detail=f"核心下载接口返回 {download.status_code}")

    filename = filename_from_content_disposition(download.headers.get("Content-Disposition", ""))
    suffix = Path(filename).suffix.lower()
    if suffix not in SUPPORTED_SUMMARY_EXTENSIONS:
        raise HTTPException(status_code=415, detail=f"暂不支持该文件类型: {suffix or 'unknown'}")

    return filename, suffix, download.content

def image_to_markdown(filename: str, content: bytes) -> str:
    with Image.open(io.BytesIO(content)) as image:
        lines = [
            f"# {filename}",
            "",
            "## Image Metadata",
            f"- Format: {image.format or 'unknown'}",
            f"- Size: {image.width} x {image.height}",
            f"- Mode: {image.mode}",
        ]

        if getattr(image, "is_animated", False):
            lines.append(f"- Animated: yes")
            lines.append(f"- Frames: {getattr(image, 'n_frames', 'unknown')}")

        exif = getattr(image, "getexif", lambda: {})()
        if exif:
            tag_names = {value: key for key, value in ExifTags.TAGS.items()}
            interesting = [
                "ImageDescription",
                "Make",
                "Model",
                "DateTime",
                "DateTimeOriginal",
                "Artist",
                "Copyright",
                "UserComment",
                "XPTitle",
                "XPComment",
                "XPKeywords",
                "GPSInfo",
            ]
            lines.extend(["", "## EXIF"])
            for name in interesting:
                tag_id = tag_names.get(name)
                if tag_id in exif:
                    value = exif.get(tag_id)
                    if isinstance(value, bytes):
                        value = value.decode("utf-8", errors="ignore").strip("\x00")
                    lines.append(f"- {name}: {value}")

        return "\n".join(lines).strip()

def text_like_to_markdown(filename: str, content: bytes) -> str:
    text = content.decode("utf-8", errors="replace")
    return f"# {filename}\n\n```text\n{text}\n```"

def media_to_markdown(filename: str, suffix: str, content: bytes) -> str:
    mime_type, _ = mimetypes.guess_type(filename)
    lines = [
        f"# {filename}",
        "",
        "## Media Metadata",
        f"- Type: {suffix.lstrip('.') or 'unknown'}",
        f"- MIME: {mime_type or 'unknown'}",
        f"- Size: {len(content)} bytes",
    ]
    return "\n".join(lines)

def fallback_markdown(filename: str, suffix: str, content: bytes) -> str:
    if suffix in IMAGE_EXTENSIONS:
        return image_to_markdown(filename, content)
    if suffix in TEXT_LIKE_EXTENSIONS:
        return text_like_to_markdown(filename, content)
    if suffix in MEDIA_EXTENSIONS:
        return media_to_markdown(filename, suffix, content)
    return ""

def convert_file_to_markdown(file_hash: str, token: str, force: bool = False):
    cache_path = markdown_cache_path(file_hash)
    filename, suffix, content = download_file_for_summary(file_hash, token)

    if cache_path.exists() and not force:
        return filename, cache_path.read_text(encoding="utf-8"), True

    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    markdown_text = ""

    try:
        if suffix in IMAGE_EXTENSIONS:
            markdown_text = fallback_markdown(filename, suffix, content)
        else:
            converter = get_markitdown_converter()
            with tempfile.NamedTemporaryFile(prefix="smartnas_", suffix=suffix, delete=True) as tmp:
                tmp.write(content)
                tmp.flush()
                try:
                    converted = converter.convert_local(tmp.name)
                except AttributeError:
                    converted = converter.convert(tmp.name)
                markdown_text = converted.text_content
    except Exception as exc:
        if suffix not in IMAGE_EXTENSIONS and suffix not in TEXT_LIKE_EXTENSIONS and suffix not in MEDIA_EXTENSIONS:
            raise HTTPException(status_code=422, detail=f"MarkItDown 转换失败: {exc}")

    if not markdown_text.strip():
        try:
            markdown_text = fallback_markdown(filename, suffix, content)
        except Exception as exc:
            raise HTTPException(status_code=422, detail=f"文件转换失败: {exc}")

    cache_path.write_text(markdown_text, encoding="utf-8")
    return filename, markdown_text, False

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
    TASK_DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(TASK_DB_PATH) as db:
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
                updated_at REAL NOT NULL
            )
            """
        )
        db.execute(
            "UPDATE agent_tasks SET status = 'failed', message = 'Agent 重启导致任务中断', "
            "error = 'interrupted', updated_at = ? WHERE status IN ('pending', 'running', 'cancel_requested')",
            (time.time(),),
        )
        rows = db.execute(
            "SELECT id, owner, file_hash, force, status, message, result_json, error, created_at, updated_at "
            "FROM agent_tasks ORDER BY created_at DESC LIMIT 500"
        ).fetchall()
    for row in reversed(rows):
        result = json.loads(row[6]) if row[6] else None
        summary_tasks[row[0]] = {
            "id": row[0], "owner": row[1], "hash": row[2], "force": bool(row[3]),
            "status": row[4], "message": row[5] or "", "result": result,
            "error": row[7], "created_at": row[8], "updated_at": row[9],
        }

def persist_summary_task(task: Dict[str, Any]) -> None:
    TASK_DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    with sqlite3.connect(TASK_DB_PATH) as db:
        db.execute(
            """
            INSERT INTO agent_tasks
                (id, owner, file_hash, force, status, message, result_json, error, created_at, updated_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(id) DO UPDATE SET
                status = excluded.status,
                message = excluded.message,
                result_json = excluded.result_json,
                error = excluded.error,
                updated_at = excluded.updated_at
            """,
            (
                task["id"], task["owner"], task["hash"], int(task.get("force", False)),
                task["status"], task.get("message", ""),
                json.dumps(task.get("result"), ensure_ascii=False) if task.get("result") is not None else None,
                task.get("error"), task["created_at"], task["updated_at"],
            ),
        )

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
        "rag": {
            "embedding_model": EMBEDDING_MODEL_NAME,
            "embedding_loaded": embedding_model is not None,
            "sentence_transformers_available": SentenceTransformer is not None,
            "sentence_transformers_error": str(SENTENCE_TRANSFORMERS_IMPORT_ERROR) if SENTENCE_TRANSFORMERS_IMPORT_ERROR else None,
            "numpy_available": np is not None,
            "numpy_error": str(NUMPY_IMPORT_ERROR) if NUMPY_IMPORT_ERROR else None,
            "faiss_available": faiss is not None,
            "faiss_error": str(FAISS_IMPORT_ERROR) if FAISS_IMPORT_ERROR else None,
            "vector_dir": str(VECTOR_DIR),
            "persistent_faiss": faiss is not None,
            "chunk_chars": RAG_CHUNK_CHARS,
            "chunk_overlap": RAG_CHUNK_OVERLAP,
            "top_k": RAG_TOP_K,
            "min_score": RAG_MIN_SCORE,
        },
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

@app.post("/api/agent/file_qa")
async def file_qa_endpoint(request: FileQuestionRequest, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    started_at = time.time()
    file_hash = request.hash.strip()
    question = request.question.strip()
    if not file_hash or not question:
        raise HTTPException(status_code=400, detail="Missing hash or question")

    filename, markdown_text, _ = convert_file_to_markdown(file_hash, token, False)
    if len(markdown_text) > FILE_QA_DIRECT_CHARS:
        if not rag_has_file_index(token, file_hash):
            index_markdown_for_rag(file_hash, filename, markdown_text, token)
        search = search_rag_chunks(question, token, max(RAG_TOP_K, 8), file_hash=file_hash)
        if not search.get("results"):
            result = {
                "status": "success", "filename": filename, "hash": file_hash,
                "mode": "rag", "answer": "没有在该文件的相关片段中找到足够依据。", "sources": [],
            }
            audit_event(owner, "file_qa", "success", file_hash=file_hash, question=question,
                        duration_ms=round((time.time() - started_at) * 1000), result=result)
            return result
        answer = answer_with_rag(question, search["results"])
        result = {
            "status": "success", "filename": filename, "hash": file_hash,
            "mode": "rag", "answer": answer,
            "sources": [
                {"chunk_index": item.get("chunk_index"), "score": item.get("score")}
                for item in search["results"]
            ],
        }
        audit_event(owner, "file_qa", "success", file_hash=file_hash, question=question,
                    duration_ms=round((time.time() - started_at) * 1000), result=result)
        return result

    messages = [
        {
            "role": "system",
            "content": "你是 SmartNAS 的文件问答助手。只能根据提供的文件内容回答；如果内容里没有答案，请明确说未在文件中找到。",
        },
        {
            "role": "user",
            "content": f"文件名：{filename}\n\n文件内容：\n{markdown_text}\n\n问题：{question}",
        },
    ]
    answer = create_chat_completion(messages)
    result = {"status": "success", "filename": filename, "hash": file_hash, "mode": "full", "answer": answer}
    audit_event(owner, "file_qa", "success", file_hash=file_hash, question=question,
                duration_ms=round((time.time() - started_at) * 1000), result=result)
    return result

@app.post("/api/agent/rag/query")
async def rag_query_endpoint(request: RagQueryRequest, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    started_at = time.time()
    query = request.query.strip()
    if not query:
        raise HTTPException(status_code=400, detail="Missing query")
    try:
        search = search_rag_chunks(query, token, request.top_k)
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
        try:
            rag_search = search_rag_chunks(prompt, token, RAG_TOP_K)
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

    try:
        rag_search = search_rag_chunks(prompt, token, RAG_TOP_K)
    except Exception:
        rag_search = {"results": []}

    def event_stream():
        answer_parts = []
        mode = "rag" if rag_search.get("results") else "chat"
        audit_event(owner, "chat_stream", "started", prompt=prompt, mode=mode)
        try:
            if rag_search.get("results"):
                final_messages = build_rag_messages(prompt, rag_search["results"])
            else:
                assistant_message = create_chat_message(history, SEARCH_FILE_TOOLS)
                if append_tool_results(history, assistant_message, token):
                    mode = "tool"
                    final_messages = history
                else:
                    content = (assistant_message.get("content") or "").strip()
                    if content:
                        answer_parts.append(content)
                        yield f"data: {json.dumps({'type': 'delta', 'content': content}, ensure_ascii=False)}\n\n"
                    final_messages = None

            if final_messages is not None:
                for content in stream_chat_completion(final_messages):
                    answer_parts.append(content)
                    yield f"data: {json.dumps({'type': 'delta', 'content': content}, ensure_ascii=False)}\n\n"
            answer = "".join(answer_parts)
            history.append({"role": "assistant", "content": answer})
            audit_event(owner, "chat_stream", "success", prompt=prompt, mode=mode,
                        duration_ms=round((time.time() - started_at) * 1000), answer=answer)
            yield f"data: {json.dumps({'type': 'done'}, ensure_ascii=False)}\n\n"
        except GeneratorExit:
            audit_event(owner, "chat_stream", "cancelled", prompt=prompt, mode=mode,
                        duration_ms=round((time.time() - started_at) * 1000), partial_answer="".join(answer_parts))
            raise
        except Exception as exc:
            detail = exc.detail if isinstance(exc, HTTPException) else str(exc)
            audit_event(owner, "chat_stream", "failed", prompt=prompt, mode=mode,
                        duration_ms=round((time.time() - started_at) * 1000),
                        partial_answer="".join(answer_parts), error=str(detail))
            yield f"data: {json.dumps({'type': 'error', 'message': str(detail)}, ensure_ascii=False)}\n\n"

    return StreamingResponse(
        event_stream(),
        media_type="text/event-stream",
        headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"},
    )

@app.post("/api/agent/clear_history")
async def clear_history(authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    owner = get_user_identity(token)
    sessions.pop(owner, None)
    audit_event(owner, "clear_history", "success")
    return {"status": "success"}

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8081)
