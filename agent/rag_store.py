import hashlib
import json
import os
import shutil
import threading
import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional

from fastapi import HTTPException

from .config import (
    EMBEDDING_MODEL_NAME,
    RAG_CHUNK_CHARS,
    RAG_CHUNK_OVERLAP,
    RAG_MIN_SCORE,
    RAG_TOP_K,
    VECTOR_DIR,
)
from .core_client import fetch_all_user_files, get_user_identity

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


embedding_model = None
embedding_generation_lock = threading.Lock()
vector_store_lock = threading.Lock()
vector_reconcile_cache: Dict[str, Dict[str, Any]] = {}
vector_reconcile_cache_lock = threading.Lock()
VECTOR_RECONCILE_TTL_SECONDS = 30
audit_callback: Optional[Callable[..., None]] = None


def set_audit_callback(callback: Callable[..., None]) -> None:
    global audit_callback
    audit_callback = callback


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


def vector_manifest_path(token: str) -> Path:
    return vector_namespace_dir(token) / "manifest.json"


def load_index_manifest(token: str) -> Dict[str, Any]:
    path = vector_manifest_path(token)
    if not path.exists():
        return {"version": 1, "embedding_model": EMBEDDING_MODEL_NAME, "files": {}}
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {"version": 1, "embedding_model": EMBEDDING_MODEL_NAME, "files": {}}
    if not isinstance(payload, dict):
        return {"version": 1, "embedding_model": EMBEDDING_MODEL_NAME, "files": {}}
    files = payload.get("files")
    if not isinstance(files, dict):
        payload["files"] = {}
    payload.setdefault("version", 1)
    payload.setdefault("embedding_model", EMBEDDING_MODEL_NAME)
    return payload


def save_index_manifest(token: str, manifest: Dict[str, Any]) -> None:
    path = vector_manifest_path(token)
    path.parent.mkdir(parents=True, exist_ok=True)
    manifest = {
        **manifest,
        "version": 1,
        "embedding_model": EMBEDDING_MODEL_NAME,
        "updated_at": int(time.time()),
        "files": manifest.get("files", {}),
    }
    path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")


def mark_file_index_status(
    token: str,
    file_hash: str,
    filename: str,
    status: str,
    *,
    chunk_count: int = 0,
    error: Optional[str] = None,
) -> None:
    manifest = load_index_manifest(token)
    files = manifest.setdefault("files", {})
    files[file_hash] = {
        "hash": file_hash,
        "filename": filename,
        "status": status,
        "chunk_count": chunk_count,
        "embedding_model": EMBEDDING_MODEL_NAME,
        "updated_at": int(time.time()),
        "error": error,
    }
    save_index_manifest(token, manifest)


def index_status(token: str) -> Dict[str, Any]:
    manifest = load_index_manifest(token)
    files = manifest.get("files", {})
    status_counts: Dict[str, int] = {}
    for item in files.values():
        status = str(item.get("status") or "unknown")
        status_counts[status] = status_counts.get(status, 0) + 1
    return {
        "embedding_model": manifest.get("embedding_model"),
        "updated_at": manifest.get("updated_at"),
        "file_count": len(files),
        "status_counts": status_counts,
        "files": list(files.values()),
    }


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
    now = time.time()
    with vector_reconcile_cache_lock:
        cached = vector_reconcile_cache.get(owner)
        if cached and now - cached.get("checked_at", 0) < VECTOR_RECONCILE_TTL_SECONDS:
            return cached["result"]

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
            manifest = load_index_manifest(token)
            files_manifest = manifest.setdefault("files", {})
            for file_hash in list(files_manifest):
                if file_hash not in active:
                    files_manifest.pop(file_hash, None)
                elif active[file_hash]:
                    files_manifest[file_hash]["filename"] = active[file_hash]
            save_index_manifest(token, manifest)
    result = {"active_files": len(active), "removed_chunks": removed, "renamed_chunks": renamed}
    with vector_reconcile_cache_lock:
        vector_reconcile_cache[owner] = {"checked_at": now, "result": result}
    if audit_callback:
        audit_callback(owner, "index_reconcile", "success", result=result)
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
        mark_file_index_status(token, file_hash, filename, "indexed", chunk_count=len(new_chunks))
    return len(new_chunks)


def search_rag_chunks(
    query: str,
    token: str,
    top_k: int = RAG_TOP_K,
    file_hash: Optional[str] = None,
    directory: Optional[str] = None,
) -> Dict[str, Any]:
    if not query.strip():
        return {"available": True, "results": [], "reason": "empty query"}

    reconcile_vector_store(token)
    with vector_store_lock:
        chunks = load_vector_chunks(token)
    chunks = [chunk for chunk in chunks if chunk.get("embedding") and chunk.get("text")]
    if file_hash:
        chunks = [chunk for chunk in chunks if chunk.get("hash") == file_hash]
    if directory:
        files = fetch_all_user_files(token)
        directory_hashes = {
            item.get("hash")
            for item in files
            if item.get("hash") and (item.get("directory") or "/") == directory
        }
        chunks = [chunk for chunk in chunks if chunk.get("hash") in directory_hashes]
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
                "source": "vector",
            }
        )
    return {"available": True, "results": results, "reason": None}


def health_status() -> Dict[str, Any]:
    return {
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
    }
