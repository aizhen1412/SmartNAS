import uvicorn
from fastapi import FastAPI, Header, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
import os
import requests
import re
import tempfile
import threading
import time
import uuid
import io
import mimetypes
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Dict, List, Optional, Any
from urllib.parse import unquote

from PIL import Image, ExifTags

try:
    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer
except Exception as exc:
    torch = None
    AutoModelForCausalLM = None
    AutoTokenizer = None
    TRANSFORMERS_IMPORT_ERROR = exc
else:
    TRANSFORMERS_IMPORT_ERROR = None

try:
    from llama_cpp import Llama
except Exception as exc:
    Llama = None
    LLAMA_CPP_IMPORT_ERROR = exc
else:
    LLAMA_CPP_IMPORT_ERROR = None

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
MODEL_PATH = os.getenv("SMARTNAS_MODEL_PATH", "models/llm/qwen2.5-7b-instruct-q4_k_m.gguf")
MAX_NEW_TOKENS = int(os.getenv("SMARTNAS_MAX_NEW_TOKENS", "512"))
MAX_MARKDOWN_CHARS = int(os.getenv("SMARTNAS_MAX_MARKDOWN_CHARS", "12000"))
SUMMARY_CHUNK_CHARS = int(os.getenv("SMARTNAS_SUMMARY_CHUNK_CHARS", "7000"))
LLAMA_CONTEXT_SIZE = int(os.getenv("SMARTNAS_LLAMA_CONTEXT_SIZE", "8192"))
CACHE_DIR = Path(os.getenv("SMARTNAS_CACHE_DIR", "var/cache/markdown"))
model_bundle = None
markitdown_converter = None
model_generation_lock = threading.Lock()
summary_executor = ThreadPoolExecutor(max_workers=1)
summary_tasks: Dict[str, Dict[str, Any]] = {}
summary_tasks_lock = threading.Lock()

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
system_prompt = """你是 SmartNAS 的智能管家。你可以通过查询数据库来帮助用户找文件。
【规则1】当用户向你询问文件内容、搜索文件时，你必须调用搜索功能，此时必须【有且仅有】回答如下格式的指令：
CALL: search_files(关键词)
【规则2】当你收到系统返回的【后台数据库查询返回】信息时，说明检索已完成。此时你必须用自然语言总结查询结果并直接回答用户。"""

class ChatRequest(BaseModel):
    prompt: str

class SummarizeRequest(BaseModel):
    hash: str
    force: bool = False

class FileQuestionRequest(BaseModel):
    hash: str
    question: str

def get_model_bundle():
    global model_bundle
    if model_bundle is not None:
        return model_bundle

    if not os.path.exists(MODEL_PATH):
        raise HTTPException(status_code=503, detail=f"模型文件或目录不存在: {MODEL_PATH}")

    if os.path.isfile(MODEL_PATH) and MODEL_PATH.lower().endswith(".gguf"):
        if Llama is None:
            raise HTTPException(status_code=503, detail=f"llama-cpp-python 加载失败: {LLAMA_CPP_IMPORT_ERROR}")

        print(f"Loading GGUF model with llama.cpp: {MODEL_PATH}")
        model = Llama(
            model_path=MODEL_PATH,
            n_ctx=LLAMA_CONTEXT_SIZE,
            n_gpu_layers=-1,
            verbose=False,
        )
        model_bundle = {"backend": "llama.cpp", "model": model}
        return model_bundle

    if not os.path.isdir(MODEL_PATH):
        raise HTTPException(status_code=503, detail=f"模型路径必须是 HuggingFace 目录或 GGUF 文件: {MODEL_PATH}")
    if AutoTokenizer is None or AutoModelForCausalLM is None or torch is None:
        raise HTTPException(status_code=503, detail=f"transformers/torch 加载失败: {TRANSFORMERS_IMPORT_ERROR}")

    print(f"Loading Transformers model: {MODEL_PATH}")
    tokenizer = AutoTokenizer.from_pretrained(
        MODEL_PATH,
        trust_remote_code=True,
    )
    model = AutoModelForCausalLM.from_pretrained(
        MODEL_PATH,
        torch_dtype="auto",
        device_map="auto",
        trust_remote_code=True,
    )
    model.eval()
    model_bundle = {"backend": "transformers", "tokenizer": tokenizer, "model": model}
    return model_bundle

def get_markitdown_converter():
    global markitdown_converter
    if markitdown_converter is not None:
        return markitdown_converter

    if MarkItDown is None:
        raise HTTPException(status_code=503, detail=f"markitdown 加载失败: {MARKITDOWN_IMPORT_ERROR}")

    markitdown_converter = MarkItDown()
    return markitdown_converter

def create_chat_completion(messages: List[dict]) -> str:
    bundle = get_model_bundle()
    with model_generation_lock:
        if bundle["backend"] == "llama.cpp":
            result = bundle["model"].create_chat_completion(
                messages=messages,
                max_tokens=MAX_NEW_TOKENS,
                temperature=0.7,
                top_p=0.8,
                repeat_penalty=1.05,
            )
            return result["choices"][0]["message"]["content"].strip()

        tokenizer = bundle["tokenizer"]
        model = bundle["model"]

        text = tokenizer.apply_chat_template(
            messages,
            tokenize=False,
            add_generation_prompt=True,
        )
        inputs = tokenizer([text], return_tensors="pt")
        inputs = {k: v.to(model.device) for k, v in inputs.items()}

        with torch.no_grad():
            generated_ids = model.generate(
                **inputs,
                max_new_tokens=MAX_NEW_TOKENS,
                do_sample=True,
                temperature=0.7,
                top_p=0.8,
                repetition_penalty=1.05,
            )

        new_tokens = generated_ids[0][inputs["input_ids"].shape[-1]:]
        return tokenizer.decode(new_tokens, skip_special_tokens=True).strip()

def require_bearer_token(authorization: Optional[str]) -> str:
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="Missing SmartNAS bearer token")
    return authorization

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

def markdown_cache_path(file_hash: str) -> Path:
    safe_hash = re.sub(r"[^a-zA-Z0-9_.-]", "_", file_hash)
    return CACHE_DIR / f"{safe_hash}.md"

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
    return f"# {filename}\n\n```text\n{text[:MAX_MARKDOWN_CHARS * 4]}\n```"

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

def run_summary(file_hash: str, token: str, force: bool = False):
    filename, markdown_text, cache_hit = convert_file_to_markdown(file_hash, token, force)
    summary = build_summary(markdown_text, filename)
    update_core_summary(file_hash, token, summary)
    return {
        "status": "success",
        "filename": filename,
        "hash": file_hash,
        "markdown_chars": len(markdown_text),
        "cache_hit": cache_hit,
        "summary": summary,
    }

def set_summary_task(task_id: str, **updates):
    with summary_tasks_lock:
        if task_id in summary_tasks:
            summary_tasks[task_id].update(updates)
            summary_tasks[task_id]["updated_at"] = time.time()

def summary_task_worker(task_id: str, file_hash: str, token: str, force: bool):
    set_summary_task(task_id, status="running", message="正在转换文件")
    try:
        result = run_summary(file_hash, token, force)
        set_summary_task(task_id, status="success", message="摘要已生成", result=result)
    except HTTPException as exc:
        set_summary_task(task_id, status="failed", message=str(exc.detail), error=str(exc.detail))
    except Exception as exc:
        set_summary_task(task_id, status="failed", message=f"摘要任务异常: {exc}", error=str(exc))

def enqueue_summary(file_hash: str, token: str, force: bool = False):
    task_id = uuid.uuid4().hex
    now = time.time()
    with summary_tasks_lock:
        summary_tasks[task_id] = {
            "id": task_id,
            "hash": file_hash,
            "status": "pending",
            "message": "等待摘要任务执行",
            "created_at": now,
            "updated_at": now,
        }
    summary_executor.submit(summary_task_worker, task_id, file_hash, token, force)
    return summary_tasks[task_id]

@app.get("/api/agent/health")
async def health():
    return {
        "status": "ok",
        "core_api": NAS_CORE_API,
        "backend": model_bundle["backend"] if model_bundle else ("llama.cpp" if MODEL_PATH.lower().endswith(".gguf") else "transformers"),
        "model_path": MODEL_PATH,
        "model_exists": os.path.exists(MODEL_PATH),
        "model_is_directory": os.path.isdir(MODEL_PATH),
        "model_is_gguf": os.path.isfile(MODEL_PATH) and MODEL_PATH.lower().endswith(".gguf"),
        "model_loaded": model_bundle is not None,
        "transformers_available": AutoModelForCausalLM is not None and AutoTokenizer is not None and torch is not None,
        "transformers_error": str(TRANSFORMERS_IMPORT_ERROR) if TRANSFORMERS_IMPORT_ERROR else None,
        "llama_cpp_available": Llama is not None,
        "llama_cpp_error": str(LLAMA_CPP_IMPORT_ERROR) if LLAMA_CPP_IMPORT_ERROR else None,
        "markitdown_available": MarkItDown is not None,
        "markitdown_error": str(MARKITDOWN_IMPORT_ERROR) if MARKITDOWN_IMPORT_ERROR else None,
        "summary_extensions": sorted(SUPPORTED_SUMMARY_EXTENSIONS),
    }

@app.post("/api/agent/summarize")
async def summarize_endpoint(request: SummarizeRequest, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    file_hash = request.hash.strip()
    if not file_hash:
        raise HTTPException(status_code=400, detail="Missing file hash")

    return run_summary(file_hash, token, request.force)

@app.post("/api/agent/summarize/start")
async def start_summarize_endpoint(request: SummarizeRequest, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    file_hash = request.hash.strip()
    if not file_hash:
        raise HTTPException(status_code=400, detail="Missing file hash")
    return enqueue_summary(file_hash, token, request.force)

@app.get("/api/agent/summarize/status/{task_id}")
async def summarize_status_endpoint(task_id: str, authorization: Optional[str] = Header(None)):
    require_bearer_token(authorization)
    with summary_tasks_lock:
        task = summary_tasks.get(task_id)
        if not task:
            raise HTTPException(status_code=404, detail="摘要任务不存在")
        return task

@app.get("/api/agent/summarize/tasks")
async def summarize_tasks_endpoint(authorization: Optional[str] = Header(None)):
    require_bearer_token(authorization)
    with summary_tasks_lock:
        return {"tasks": list(summary_tasks.values())[-100:]}

@app.post("/api/agent/summarize/missing")
async def summarize_missing_endpoint(authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    try:
        response = requests.get(
            f"{NAS_CORE_API}/api/list",
            headers={"Authorization": token},
            timeout=20,
        )
    except Exception as exc:
        raise HTTPException(status_code=502, detail=f"无法读取文件列表: {exc}")

    if response.status_code != 200:
        raise HTTPException(status_code=response.status_code, detail=f"核心列表接口返回 {response.status_code}")

    tasks = []
    payload = response.json()
    file_items = payload.get("files", payload if isinstance(payload, list) else [])
    for file_info in file_items:
        name = file_info.get("name", "")
        file_hash = file_info.get("hash", "")
        summary = (file_info.get("summary") or "").strip()
        if summary or not file_hash:
            continue
        if Path(name).suffix.lower() not in SUPPORTED_SUMMARY_EXTENSIONS:
            continue
        tasks.append(enqueue_summary(file_hash, token, False))

    return {"status": "success", "queued": len(tasks), "tasks": tasks}

@app.get("/api/agent/markdown/{file_hash}")
async def markdown_endpoint(file_hash: str, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    filename, markdown_text, cache_hit = convert_file_to_markdown(file_hash, token, False)
    return {
        "status": "success",
        "filename": filename,
        "hash": file_hash,
        "cache_hit": cache_hit,
        "markdown": markdown_text,
    }

@app.post("/api/agent/file_qa")
async def file_qa_endpoint(request: FileQuestionRequest, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    file_hash = request.hash.strip()
    question = request.question.strip()
    if not file_hash or not question:
        raise HTTPException(status_code=400, detail="Missing hash or question")

    filename, markdown_text, _ = convert_file_to_markdown(file_hash, token, False)
    context = markdown_text[:MAX_MARKDOWN_CHARS * 2]
    messages = [
        {
            "role": "system",
            "content": "你是 SmartNAS 的文件问答助手。只能根据提供的文件内容回答；如果内容里没有答案，请明确说未在文件中找到。",
        },
        {
            "role": "user",
            "content": f"文件名：{filename}\n\n文件内容：\n{context}\n\n问题：{question}",
        },
    ]
    answer = create_chat_completion(messages)
    return {"status": "success", "filename": filename, "hash": file_hash, "answer": answer}

@app.post("/api/agent/chat")
async def chat_endpoint(request: ChatRequest, authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    history = sessions.setdefault(token, [])

    if not history:
        history.append({"role": "system", "content": system_prompt})

    if len(history) > 10:
        # 简单截断防止超出上下文
        sessions[token] = [history[0]] + history[-5:]
        history = sessions[token]

    history.append({"role": "user", "content": request.prompt})

    try:
        text = create_chat_completion(history)

        match = re.search(r"CALL:\s*search_files\((.*?)\)", text)
        if match:
            keyword = match.group(1).strip("'\"")
            print(f"[Tool] Searching files for: {keyword}")
            try:
                r = requests.get(
                    f"{NAS_CORE_API}/api/v1/files/search",
                    params={"keyword": keyword},
                    headers={"Authorization": token},
                    timeout=10,
                )
                if r.status_code == 200:
                    data = r.json()
                    db_result = f"【后台数据库查询返回】共找到 {len(data)} 个文件。\n"
                    for f in data:
                        db_result += f"- {f.get('filename')} (摘要: {f.get('summary')})\n"
                    if not data:
                        db_result += "未找到任何记录。"
                else:
                    db_result = f"核心 API 错误: {r.status_code}"
            except Exception as e:
                db_result = f"无法连接到核心服务: {e}"

            sys_msg = f"【系统消息】查询完成。结果如下：\n{db_result}\n请根据此结果使用自然语言回答用户的原始问题，绝不要再输出CALL指令。"
            history.append({"role": "assistant", "content": text})
            history.append({"role": "system", "content": sys_msg})

            final_text = create_chat_completion(history)
            history.append({"role": "assistant", "content": final_text})

            return {"status": "success", "response": final_text}
        else:
            history.append({"role": "assistant", "content": text})
            return {"status": "success", "response": text}
    except HTTPException:
        raise
    except Exception as e:
        return {"status": "error", "message": f"Agent 执行异常: {str(e)}"}

@app.post("/api/agent/clear_history")
async def clear_history(authorization: Optional[str] = Header(None)):
    token = require_bearer_token(authorization)
    sessions.pop(token, None)
    return {"status": "success"}

if __name__ == "__main__":
    uvicorn.run(app, host="0.0.0.0", port=8081)
