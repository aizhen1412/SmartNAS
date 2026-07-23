import json
import sqlite3
import time
from typing import Any, Dict, List, Optional

from .core_client import fetch_all_user_files
from .rag_store import load_vector_chunks, vector_namespace_dir


keyword_index_cache: Dict[str, Dict[str, Any]] = {}


def keyword_db_path(token: str):
    return vector_namespace_dir(token) / "keyword.sqlite"


def _metadata_by_hash(token: str) -> Dict[str, Dict[str, Any]]:
    return {item.get("hash"): item for item in fetch_all_user_files(token) if item.get("hash")}


def _tags_text(value: Any) -> str:
    if isinstance(value, list):
        return " ".join(str(item) for item in value)
    if not isinstance(value, str):
        return ""
    try:
        parsed = json.loads(value)
    except (TypeError, ValueError):
        return value
    if isinstance(parsed, list):
        return " ".join(str(item) for item in parsed)
    return value


def _connect(token: str):
    path = keyword_db_path(token)
    path.parent.mkdir(parents=True, exist_ok=True)
    return sqlite3.connect(path)


def _create_schema(db) -> bool:
    db.execute(
        """
        CREATE TABLE IF NOT EXISTS keyword_index_meta (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL
        )
        """
    )
    db.execute(
        """
        CREATE TABLE IF NOT EXISTS keyword_chunks (
            chunk_id TEXT PRIMARY KEY,
            hash TEXT NOT NULL,
            filename TEXT NOT NULL,
            chunk_index INTEGER NOT NULL,
            directory TEXT NOT NULL,
            summary TEXT NOT NULL,
            tags TEXT NOT NULL,
            text TEXT NOT NULL
        )
        """
    )
    db.execute("CREATE INDEX IF NOT EXISTS idx_keyword_hash ON keyword_chunks(hash)")
    db.execute("CREATE INDEX IF NOT EXISTS idx_keyword_directory ON keyword_chunks(directory)")
    try:
        db.execute(
            """
            CREATE VIRTUAL TABLE IF NOT EXISTS keyword_chunks_fts USING fts5(
                chunk_id UNINDEXED,
                filename,
                directory,
                summary,
                tags,
                text,
                tokenize='unicode61'
            )
            """
        )
    except sqlite3.Error:
        return False
    return True


def _set_meta(db, key: str, value: Any) -> None:
    db.execute(
        """
        INSERT INTO keyword_index_meta (key, value)
        VALUES (?, ?)
        ON CONFLICT(key) DO UPDATE SET value = excluded.value
        """,
        (key, json.dumps(value, ensure_ascii=False)),
    )


def _get_meta(db, key: str, default: Any = None) -> Any:
    row = db.execute("SELECT value FROM keyword_index_meta WHERE key = ?", (key,)).fetchone()
    if not row:
        return default
    try:
        return json.loads(row[0])
    except (TypeError, ValueError):
        return default


def keyword_index_status(token: str) -> Dict[str, Any]:
    with _connect(token) as db:
        fts_available = _create_schema(db)
        row_count = db.execute("SELECT COUNT(*) FROM keyword_chunks").fetchone()[0]
        return {
            "dirty": bool(_get_meta(db, "dirty", row_count == 0)),
            "reason": _get_meta(db, "reason", "empty" if row_count == 0 else None),
            "updated_at": _get_meta(db, "updated_at"),
            "indexed_chunks": int(_get_meta(db, "indexed_chunks", row_count)),
            "row_count": row_count,
            "fts_available": fts_available,
        }


def mark_keyword_index_dirty(token: str, reason: str = "vector index changed") -> None:
    with _connect(token) as db:
        _create_schema(db)
        _set_meta(db, "dirty", True)
        _set_meta(db, "reason", reason)
    keyword_index_cache.pop(token, None)


def rebuild_keyword_index(token: str) -> Dict[str, int]:
    metadata = _metadata_by_hash(token)
    chunks = load_vector_chunks(token)
    rows = []
    for chunk in chunks:
        file_hash = chunk.get("hash")
        if not file_hash or file_hash not in metadata:
            continue
        meta = metadata[file_hash]
        rows.append(
            {
                "chunk_id": chunk.get("id") or f"{file_hash}:{chunk.get('chunk_index', 0)}",
                "hash": file_hash,
                "filename": meta.get("name") or chunk.get("filename") or "",
                "chunk_index": int(chunk.get("chunk_index") or 0),
                "directory": meta.get("directory") or "/",
                "summary": meta.get("summary") or "",
                "tags": _tags_text(meta.get("tags")),
                "text": chunk.get("text") or "",
            }
        )

    with _connect(token) as db:
        fts_available = _create_schema(db)
        db.execute("DELETE FROM keyword_chunks")
        if fts_available:
            db.execute("DELETE FROM keyword_chunks_fts")
        for row in rows:
            db.execute(
                """
                INSERT INTO keyword_chunks
                    (chunk_id, hash, filename, chunk_index, directory, summary, tags, text)
                VALUES
                    (:chunk_id, :hash, :filename, :chunk_index, :directory, :summary, :tags, :text)
                """,
                row,
            )
            if fts_available:
                db.execute(
                    """
                    INSERT INTO keyword_chunks_fts
                        (chunk_id, filename, directory, summary, tags, text)
                    VALUES
                        (:chunk_id, :filename, :directory, :summary, :tags, :text)
                    """,
                    row,
                )
        _set_meta(db, "dirty", False)
        _set_meta(db, "reason", None)
        _set_meta(db, "updated_at", int(time.time()))
        _set_meta(db, "indexed_chunks", len(rows))
        _set_meta(db, "fts_available", fts_available)
    result = {"indexed_chunks": len(rows), "fts_available": fts_available}
    keyword_index_cache[token] = {"checked_at": time.time(), **result, "dirty": False}
    return result


def ensure_keyword_index(token: str) -> None:
    cached = keyword_index_cache.get(token)
    if cached and not cached.get("dirty"):
        return
    status = keyword_index_status(token)
    if not status["dirty"] and status["row_count"] > 0:
        keyword_index_cache[token] = {"checked_at": time.time(), **status}
        return
    rebuild_keyword_index(token)


def _match_query(query: str) -> str:
    terms = [term.strip() for term in query.replace('"', " ").split() if term.strip()]
    if not terms:
        terms = [query.strip()]
    return " OR ".join(f'"{term}"' for term in terms if term)


def _like_query(query: str) -> str:
    return f"%{query.strip()}%"


def _row_to_result(row, score: float, source: str) -> Dict[str, Any]:
    return {
        "score": round(score, 4),
        "hash": row[1],
        "filename": row[2],
        "chunk_index": row[3],
        "text": row[7],
        "source": source,
        "keyword": True,
    }


def search_keyword_chunks(
    query: str,
    token: str,
    top_k: int,
    *,
    file_hash: Optional[str] = None,
    directory: Optional[str] = None,
) -> Dict[str, Any]:
    if not query.strip():
        return {"available": True, "results": [], "reason": "empty query"}

    ensure_keyword_index(token)
    limit = max(1, top_k)
    params: List[Any] = []
    where = []
    if file_hash:
        where.append("c.hash = ?")
        params.append(file_hash)
    if directory:
        where.append("c.directory = ?")
        params.append(directory)
    where_sql = f" AND {' AND '.join(where)}" if where else ""

    results: List[Dict[str, Any]] = []
    with _connect(token) as db:
        fts_available = _create_schema(db)
        if fts_available:
            try:
                match = _match_query(query)
                rows = db.execute(
                    f"""
                    SELECT c.chunk_id, c.hash, c.filename, c.chunk_index, c.directory,
                           c.summary, c.tags, c.text, bm25(keyword_chunks_fts) AS rank
                    FROM keyword_chunks_fts f
                    JOIN keyword_chunks c ON c.chunk_id = f.chunk_id
                    WHERE keyword_chunks_fts MATCH ?{where_sql}
                    ORDER BY rank
                    LIMIT ?
                    """,
                    [match, *params, limit],
                ).fetchall()
                for rank, row in enumerate(rows):
                    results.append(_row_to_result(row, 0.72 - min(rank, 20) * 0.02, "keyword"))
            except sqlite3.Error:
                results = []
        else:
            results = []

        if len(results) < limit:
            like = _like_query(query)
            rows = db.execute(
                f"""
                SELECT chunk_id, hash, filename, chunk_index, directory, summary, tags, text
                FROM keyword_chunks c
                WHERE (
                    c.filename LIKE ? OR c.directory LIKE ? OR c.summary LIKE ? OR
                    c.tags LIKE ? OR c.text LIKE ?
                ){where_sql}
                LIMIT ?
                """,
                [like, like, like, like, like, *params, limit],
            ).fetchall()
            seen = {(item["hash"], item["chunk_index"]) for item in results}
            for row in rows:
                key = (row[1], row[3])
                if key in seen:
                    continue
                seen.add(key)
                score = 0.58 if query in " ".join(str(value) for value in row[2:7]) else 0.46
                results.append(_row_to_result(row, score, "keyword_like"))
                if len(results) >= limit:
                    break

    return {"available": True, "results": results[:limit], "reason": None}
